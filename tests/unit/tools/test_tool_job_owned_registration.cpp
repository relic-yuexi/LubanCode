#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "agent/loop.hpp"
#include "hooks/hash.hpp"
#include "platform/paths.hpp"
#include "tools/registry.hpp"
#include "tools/tool_job_coordinator.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

namespace {
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
namespace agent = lubancode::agent;
namespace runtime = lubancode::runtime;
using namespace lubancode::tools;
using Json = nlohmann::json;

struct OwnedDirectory {
    fs::path path;
    explicit OwnedDirectory(const std::string& tag) {
        static std::atomic<unsigned> sequence{0};
        try {
            for (unsigned attempt = 0; attempt != 64; ++attempt) {
                auto candidate = fs::temp_directory_path() / ("lubancode-job-registration-" + tag + "-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                    std::to_string(sequence.fetch_add(1)));
                std::error_code error;
                const bool created = fs::create_directory(candidate, error);
                if (error == std::errc::file_exists) continue;
                REQUIRE_MESSAGE(!error, error.message());
                if (created) {
                    path = std::move(candidate);
                    const auto canonical = fs::canonical(path, error);
                    REQUIRE_MESSAGE(!error, error.message());
                    path = canonical;
                    break;
                }
            }
            REQUIRE_FALSE(path.empty());
        } catch (...) {
            if (!path.empty()) { std::error_code error; fs::remove_all(path, error); }
            throw;
        }
    }
    ~OwnedDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
        try { CHECK_MESSAGE(!error, error.message()); } catch (...) {}
    }
};

std::string Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    REQUIRE_FALSE(input.bad());
    return bytes;
}
v3::V3Ledger Read(const fs::path& path) {
    auto ledger = v3::ReadV3Ledger(path);
    REQUIRE_MESSAGE(ledger.has_value(), (ledger ? std::string() : ledger.error()));
    return std::move(*ledger);
}
void Committed(const v3::WriteReceipt& receipt) {
    REQUIRE_MESSAGE(receipt.status == v3::WriteReceipt::Status::Committed, receipt.error_message);
    REQUIRE_FALSE(receipt.id.empty()); REQUIRE(receipt.seq > 0); REQUIRE_FALSE(receipt.line_hash.empty());
}
std::string Hash(const Json& value) {
    const auto bytes = lubancode::trajectory::CanonicalJsonDump(value);
    REQUIRE(bytes.has_value());
    return lubancode::hooks::Sha256Hex(*bytes);
}
void Mark(const char* path) {
    std::fprintf(stderr, "[job-owned-registration-path] %s\n", path);
    std::fflush(stderr);
}

struct Counts {
    std::atomic<unsigned> execute{0}, gate{0}, clock{0}, thread{0}, pre{0}, permission{0};
};
struct Fault {
    unsigned fail_at = 0, writes = 0;
    std::optional<std::string> Next() {
        if (fail_at && ++writes == fail_at) return "registration-write-fault";
        return std::nullopt;
    }
};
class Probe final : public Tool {
public:
    explicit Probe(std::shared_ptr<Counts> counts) : counts_(std::move(counts)) {}
    std::string name() const override { return "registration_probe"; }
    std::string description() const override { return "real preparation without dispatch"; }
    Json input_schema() const override {
        return {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}},
            {"required", Json::array({"path"})}, {"additionalProperties", false}};
    }
    bool needs_confirm() const override { return true; }
    ApprovalClass approval_class() const override { return ApprovalClass::FileEdit; }
    Result execute(const Json&) override { ++counts_->execute; return {"executed", false}; }
private:
    std::shared_ptr<Counts> counts_;
};

struct Source {
    lubancode::api::ToolUseBlock call;
    std::string message, admission, pending, action;
};
struct Rig {
    OwnedDirectory root;
    fs::path journal;
    std::shared_ptr<Counts> counts = std::make_shared<Counts>();
    std::shared_ptr<Fault> fault = std::make_shared<Fault>();
    std::shared_ptr<std::recursive_mutex> serial = std::make_shared<std::recursive_mutex>();
    std::shared_ptr<GlobalRunningQuota> quota = std::make_shared<GlobalRunningQuota>();
    std::optional<v3::V3Writer> writer;
    std::shared_ptr<ToolJobCoordinator> coordinator;

    explicit Rig(const std::string& tag, std::size_t capacity = 8) : root(tag), journal(root.path / "s1.jsonl") {
        v3::V3WriterOptions writer_options;
        writer_options.inject_io_failure = [owner = fault] { return owner->Next(); };
        auto opened = v3::V3Writer::Start(journal, "20261003-120000-JREG", "run-000001", "registration fixture", Json::object(), writer_options);
        REQUIRE_MESSAGE(opened.has_value(), (opened ? std::string() : opened.error()));
        writer = std::move(*opened);
        ToolJobCoordinator::Options options;
        options.limits.queued_max = capacity; options.global = quota;
        options.prepared_registration = PreparedRegistrationContext{serial, "project-fixture", root.path};
        options.clock_ms = [owner = counts] { ++owner->clock; return std::int64_t{1}; };
        options.thread_starter = [owner = counts](std::thread& thread, std::function<void()> body) {
            ++owner->thread; thread = std::thread(std::move(body));
        };
        coordinator = std::make_shared<ToolJobCoordinator>(*writer,
            [owner = counts](const std::string&, const Json&) { ++owner->gate; return JobAuthDecision{true, false, {}}; },
            [owner = counts](const JobExecutionContext&) { ++owner->execute; return Tool::Result{"executed", false}; }, options);
    }
    ~Rig() { coordinator.reset(); writer.reset(); }

    Source Declare(unsigned number = 1, bool admitted = true, bool pending = true) {
        std::lock_guard lock(*serial);
        Source source;
        source.call.id = "provider-" + std::to_string(number);
        source.call.name = "registration_probe";
        source.call.input = {{"path", "original-" + std::to_string(number) + ".txt"}};
        source.action = "action-parent-" + std::to_string(number);
        v3::MessageDraft message;
        message.turn_id = "turn-000001"; message.step_id = "step-000001"; message.request_id = "request-000001";
        message.origin = v3::MessageOrigin::SessionRuntime;
        message.provider = "fixture"; message.wire = "responses"; message.model = "fixture";
        message.response_model = "fixture"; message.usage = {{"inputTokens", 1}, {"outputTokens", 1}};
        message.message = {{"role", "assistant"}, {"content", "declared"},
            {"tool_calls", Json::array({{{"id", source.call.id}, {"type", "function"},
                {"function", {{"name", source.call.name}, {"arguments", source.call.input.dump()}}}}})}};
        const auto written = writer->AppendMessage(std::move(message), v3::Durability::PowerLoss);
        Committed(written); source.message = written.id;
        if (admitted) {
            const auto applied = writer->AdmitMessages({written.id}); Committed(applied); source.admission = applied.id;
        }
        if (pending) {
            const auto action = v3::ToolActionSession::Admit(*writer, "turn-000001", "step-000001", source.action,
                "prepared-parent", source.message, source.call.id, Json::object(), v3::Durability::PowerLoss);
            REQUIRE(action.last_event_id().has_value()); source.pending = *action.last_event_id();
        }
        return source;
    }
    PreparedJobRequest Prepare(const Source& source, bool rewrite = false) {
        ToolRegistry registry; ToolRegistration registration;
        registration.tool = std::make_unique<Probe>(counts);
        registration.source_kind = lubancode::ToolSourceKind::Builtin;
        registration.source_instance = "registration-fixture"; registration.version_or_digest = "fixture-v1";
        registration.effect_class = lubancode::EffectClass::ReadOnlyLocal;
        registry.Register(std::move(registration));
        agent::TurnWiring wiring;
        wiring.on_pre_tool_use_hook = [owner = counts, rewrite](const auto&, const auto&, const auto&) {
            ++owner->pre; runtime::ToolHookDecision pre; pre.decision = runtime::ToolHookDecision::Decision::Allow;
            if (rewrite) pre.updated_input = Json{{"path", "effective.txt"}};
            return pre;
        };
        wiring.on_permission_evaluate = [owner = counts, rewrite, &source](const auto& id, const auto& name, auto, const auto& input, const auto&) {
            ++owner->permission; CHECK(id == source.call.id); CHECK(name == source.call.name);
            const Json expected = rewrite ? Json{{"path", "effective.txt"}} : source.call.input;
            CHECK(bool(input == expected));
            runtime::PermissionVerdict permission; permission.action = runtime::PermissionVerdict::Action::Allow; return permission;
        };
        const auto prepared = agent::PrepareOwnedToolInput(registry, source.call, wiring, {});
        REQUIRE_MESSAGE(prepared.has_value(), (prepared ? std::string() : prepared.error().content));
        const auto owner = coordinator->PreparedOwner(); REQUIRE(owner.has_value());
        PreparedJobRequest request;
        request.owner = *owner; request.provider_tool_call_id = prepared->call_id;
        request.tool_name = prepared->tool_name; request.original_input = source.call.input;
        request.effective_input = prepared->effective_input;
        request.assistant_message_ref = source.message; request.parent_action_id = source.action;
        request.turn_id = "turn-000001"; request.step_id = "step-000001";
        request.tool_identity = {prepared->tool_name, prepared->source_instance, "fixture-v1",
            lubancode::platform::PathToUtf8(owner->cwd)};
        request.policy.allow_background = true;
        request.policy.side_effect_class = "read_only";
        return request;
    }
    void Idle() const {
        CHECK(coordinator->running_count() == 0); CHECK(coordinator->queued_count() == 0);
        CHECK(quota->running.load() == 0); CHECK(counts->execute.load() == 0);
        CHECK(counts->gate.load() == 0); CHECK(counts->clock.load() == 0); CHECK(counts->thread.load() == 0);
    }
};

void ReceiptInLedger(const v3::V3Ledger& ledger, const v3::WriteReceipt& receipt, v3::EventKindV3 kind) {
    Committed(receipt);
    const auto* event = ledger.FindEvent(receipt.id); REQUIRE(event != nullptr);
    CHECK(event->kind == kind); CHECK(event->seq == receipt.seq); CHECK(event->line_hash == receipt.line_hash);
}
void StagedOnly(const v3::V3Ledger& ledger, const PreparedJobFacts& facts) {
    REQUIRE(facts.pending_receipt.has_value()); REQUIRE(facts.registered_receipt.has_value());
    ReceiptInLedger(ledger, *facts.pending_receipt, v3::EventKindV3::ToolExecutionPending);
    ReceiptInLedger(ledger, *facts.registered_receipt, v3::EventKindV3::ToolJobRegistered);
    REQUIRE(facts.pending_receipt->seq < facts.registered_receipt->seq);
    const auto* pending = ledger.FindEvent(facts.source_pending_event_id); REQUIRE(pending != nullptr);
    const auto* applied = ledger.FindEvent(facts.source_admission_event_id); REQUIRE(applied != nullptr);
    CHECK(pending->action_id == facts.parent_action_id); CHECK(applied->kind == v3::EventKindV3::ContextInputApplied);
    const auto* declaration = ledger.FindMessage(facts.assistant_message_ref); REQUIRE(declaration != nullptr);
    CHECK(declaration->session_id == facts.owner.session_id); CHECK(declaration->run_id == facts.owner.run_id);
    const auto& call = declaration->message.at("tool_calls").at(0);
    CHECK(call.at("id").get<std::string>() == facts.provider_tool_call_id);
    const auto original = Json::parse(call.at("function").at("arguments").get<std::string>());
    CHECK(bool(original == facts.original_input));
    CHECK(facts.original_input_sha256 == Hash(original)); CHECK(facts.effective_input_sha256 == Hash(facts.effective_input));
    const auto* registered = ledger.FindEvent(facts.registered_receipt->id); REQUIRE(registered != nullptr);
    CHECK(registered->session_id == facts.owner.session_id); CHECK(registered->run_id == facts.owner.run_id);
    CHECK(registered->turn_id == facts.turn_id); CHECK(registered->step_id == facts.step_id);
    CHECK(registered->action_id == facts.action_id); CHECK(registered->payload.at("preparedOnly").get<bool>());
    CHECK(registered->payload.at("jobId").get<std::string>() == facts.job_id);
    CHECK(registered->payload.at("provider_tool_call_id").get<std::string>() == facts.provider_tool_call_id);
    CHECK(registered->payload.at("parentActionId").get<std::string>() == facts.parent_action_id);
    CHECK(registered->payload.at("sourcePendingEventRef").get<std::string>() == facts.source_pending_event_id);
    CHECK(registered->payload.at("sourceAdmissionEventRef").get<std::string>() == facts.source_admission_event_id);
    CHECK(bool(registered->payload.at("effectiveInput") == facts.effective_input));
    CHECK(registered->payload.at("originalInputSha256").get<std::string>() == facts.original_input_sha256);
    CHECK(registered->payload.at("effectiveInputSha256").get<std::string>() == facts.effective_input_sha256);
    CHECK(bool(registered->payload.at("toolIdentity") == facts.tool_identity.ToJson()));
    CHECK(bool(registered->payload.at("executionPolicy") == facts.policy.ToJson()));
    const auto& owner = registered->payload.at("preparedOwner");
    CHECK(owner.at("sessionId").get<std::string>() == facts.owner.session_id);
    CHECK(owner.at("runId").get<std::string>() == facts.owner.run_id);
    CHECK(owner.at("coordinatorId").get<std::uint64_t>() == facts.owner.coordinator_id);
    CHECK(owner.at("epoch").get<std::uint64_t>() == facts.owner.epoch);
    CHECK(owner.at("projectId").get<std::string>() == facts.owner.project_id);
    CHECK(owner.at("cwd").get<std::string>() == lubancode::platform::PathToUtf8(facts.owner.cwd));
    for (const auto& event : ledger.events) {
        if (event.action_id != facts.action_id) continue;
        const bool allowed = event.kind == v3::EventKindV3::ToolExecutionPending || event.kind == v3::EventKindV3::ToolJobRegistered;
        CHECK(allowed);
    }
    CHECK(ledger.messages.size() == 2); // Initial system and the real assistant; no admission tool message.
}
void RejectUnchanged(Rig& rig, const PreparedJobRequest& request, const std::string& error_code = {}) {
    const auto before = Bytes(rig.journal);
    const auto count = rig.coordinator->prepared_count();
    const auto result = rig.coordinator->RegisterPreparedJob(request);
    CHECK(result.state == PreparedJobRegistrationState::Rejected); CHECK_FALSE(result.error_code.empty());
    if (!error_code.empty()) CHECK(result.error_code == error_code);
    CHECK_FALSE(result.facts); CHECK(rig.coordinator->prepared_count() == count);
    CHECK(Bytes(rig.journal) == before); rig.Idle();
}
void ShutdownUnderSerial(Rig& rig) {
    const auto before = Bytes(rig.journal);
    REQUIRE(rig.coordinator->PreparedOwner().has_value());
    std::unique_lock lock(*rig.serial);
    std::future<bool> finished;
    struct ReleaseThenJoin {
        std::unique_lock<std::recursive_mutex>& lock;
        std::future<bool>& finished;
        ~ReleaseThenJoin() {
            if (lock.owns_lock()) lock.unlock();
            if (finished.valid()) finished.wait();
        }
    } cleanup{lock, finished};
    finished = std::async(std::launch::async, [coordinator = rig.coordinator] { return coordinator->Shutdown(); });
    // The recursive host lock permits this same thread's query. A lost owner
    // while the actual writer is still open proves Shutdown published closing.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (rig.coordinator->PreparedOwner() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE_FALSE(rig.coordinator->PreparedOwner());
    CHECK(finished.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    CHECK_FALSE(rig.coordinator->shutdown_complete()); CHECK_FALSE(rig.writer->closed());
    CHECK(Bytes(rig.journal) == before); rig.Idle();
    lock.unlock();
    REQUIRE(finished.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(finished.get()); CHECK(rig.coordinator->shutdown_complete());
    CHECK(Bytes(rig.journal) == before); rig.Idle();
}
} // namespace

TEST_CASE("Owned Job registration persists only native temporary facts without dispatch") {
    Rig rig("stage"); const auto source = rig.Declare(); auto request = rig.Prepare(source);
    const auto result = rig.coordinator->RegisterPreparedJob(request);
    REQUIRE_MESSAGE(result.state == PreparedJobRegistrationState::Registered, result.error);
    REQUIRE(result.facts); CHECK(result.error_code.empty()); CHECK(result.error.empty());
    CHECK(result.facts->owner == request.owner); CHECK(result.facts->attempt == 1);
    CHECK(result.facts->source_pending_event_id == source.pending); CHECK(result.facts->source_admission_event_id == source.admission);
    CHECK(rig.coordinator->prepared_count() == 1); rig.Idle();
    StagedOnly(Read(rig.journal), *result.facts);
    const auto before = Bytes(rig.journal);
    for (unsigned i = 0; i != 3; ++i) {
        const auto view = rig.coordinator->GetPreparedJob(request.owner, result.facts->job_id);
        REQUIRE(view.has_value()); CHECK(view->facts == result.facts); CHECK_FALSE(view->revoked);
        CHECK(view->state == PreparedJobRegistrationState::Registered);
    }
    CHECK(Bytes(rig.journal) == before); rig.Idle(); Mark("stage");
}

TEST_CASE("Owned Job registration retains shared Prepare rewrite and independently read original declaration") {
    Rig rig("rewrite"); auto source = rig.Declare(); auto request = rig.Prepare(source, true);
    REQUIRE(request.original_input.at("path").get<std::string>() != request.effective_input.at("path").get<std::string>());
    const auto result = rig.coordinator->RegisterPreparedJob(request);
    REQUIRE_MESSAGE(result.state == PreparedJobRegistrationState::Registered, result.error); REQUIRE(result.facts);
    StagedOnly(Read(rig.journal), *result.facts);
    CHECK(result.facts->original_input_sha256 != result.facts->effective_input_sha256);
    CHECK(result.facts->effective_input.at("path").get<std::string>() == "effective.txt");
    CHECK(rig.counts->pre.load() == 1); CHECK(rig.counts->permission.load() == 1); rig.Idle();
    const auto owner = request.owner; const auto job = result.facts->job_id;
    source.call.input["path"] = "caller-mutated"; request.original_input = Json::object();
    request.effective_input = Json::object(); request.owner.session_id = "caller-mutated";
    const auto view = rig.coordinator->GetPreparedJob(owner, job); REQUIRE(view.has_value());
    CHECK(view->facts == result.facts); CHECK(view->facts->original_input.at("path").get<std::string>() == "original-1.txt");
    CHECK(view->facts->effective_input.at("path").get<std::string>() == "effective.txt");
    REQUIRE(rig.coordinator->Shutdown()); rig.coordinator.reset();
    CHECK(result.facts->owner == owner); CHECK(result.facts->effective_input.at("path").get<std::string>() == "effective.txt");
    Mark("rewrite");
}

TEST_CASE("Owned Job registration rejects missing declaration and foreign actual owner without writes") {
    Rig rig("owner"); const auto source = rig.Declare(); const auto valid = rig.Prepare(source);
    for (unsigned variant = 0; variant != 21; ++variant) {
        auto request = valid;
        switch (variant) {
        case 0: request.provider_tool_call_id.clear(); break;
        case 1: request.assistant_message_ref.clear(); break;
        case 2: request.parent_action_id.clear(); break;
        case 3: request.turn_id.clear(); break;
        case 4: request.step_id.clear(); break;
        case 5: request.owner.session_id += "foreign"; break;
        case 6: request.owner.run_id += "foreign"; break;
        case 7: ++request.owner.coordinator_id; break;
        case 8: ++request.owner.epoch; break;
        case 9: request.owner.project_id += "foreign"; break;
        case 10: request.owner.cwd = rig.root.path.parent_path(); break;
        case 11: request.original_input["path"] = "not-in-declaration.txt"; break;
        case 12: request.original_input = Json::array(); break;
        case 13: request.effective_input = Json::array(); break;
        case 14: request.tool_name = "foreign_probe"; break;
        case 15: request.provider_tool_call_id = "other-provider"; break;
        case 16: request.turn_id = "turn-foreign"; break;
        case 17: request.step_id = "step-foreign"; break;
        case 18: request.tool_identity.execution_scope = "not-the-cwd"; break;
        case 19: request.policy.allow_background = false; break;
        default: request.policy.resume_policy = "requeue_when_registered"; break;
        }
        INFO("variant=" << variant); RejectUnchanged(rig, request);
    }
    { Rig missing("unadmitted"); const auto undeclared = missing.Declare(1, false);
      RejectUnchanged(missing, missing.Prepare(undeclared)); }
    { Rig missing("no-pending"); const auto undeclared = missing.Declare(1, true, false);
      RejectUnchanged(missing, missing.Prepare(undeclared)); }
    {
        Rig repeated("multiple-admissions"); const auto declaration = repeated.Declare();
        const auto again = repeated.writer->AdmitMessages({declaration.message}); Committed(again);
        const auto readable = Read(repeated.journal);
        unsigned matching = 0;
        for (const auto& event : readable.events) {
            if (event.kind != v3::EventKindV3::ContextInputApplied) continue;
            for (const auto& id : event.payload.at("addedMessageRefs"))
                if (id.get<std::string>() == declaration.message) ++matching;
        }
        REQUIRE(matching == 2);
        RejectUnchanged(repeated, repeated.Prepare(declaration), "job.prepared.invalid_source");
    }
    {
        Rig late("late-admission"); const auto declaration = late.Declare(1, false, true);
        const auto admitted = late.writer->AdmitMessages({declaration.message}); Committed(admitted);
        const auto readable = Read(late.journal);
        const auto* pending = readable.FindEvent(declaration.pending); REQUIRE(pending != nullptr);
        REQUIRE(pending->seq < admitted.seq);
        RejectUnchanged(late, late.Prepare(declaration), "job.prepared.invalid_source");
    }
    Rig peer("peer"); const auto peer_source = peer.Declare(); const auto peer_request = peer.Prepare(peer_source);
    RejectUnchanged(peer, valid); RejectUnchanged(rig, peer_request);
    const auto registered = rig.coordinator->RegisterPreparedJob(valid); REQUIRE(registered.facts);
    CHECK_FALSE(peer.coordinator->GetPreparedJob(valid.owner, registered.facts->job_id));
    auto foreign = valid.owner; ++foreign.epoch;
    CHECK_FALSE(rig.coordinator->GetPreparedJob(foreign, registered.facts->job_id));
    CHECK_FALSE(rig.coordinator->GetPreparedJob(valid.owner, "job-never-allocated"));
    rig.Idle(); peer.Idle(); Mark("owner");
}

TEST_CASE("Owned Job registration reserves real finite capacity and rejects duplicate declaration") {
    Rig rig("capacity", 1); const auto first = rig.Declare(); const auto second = rig.Declare(2);
    auto request = rig.Prepare(first); const auto other = rig.Prepare(second);
    const auto registered = rig.coordinator->RegisterPreparedJob(request);
    REQUIRE_MESSAGE(registered.state == PreparedJobRegistrationState::Registered, registered.error); REQUIRE(registered.facts);
    RejectUnchanged(rig, request); RejectUnchanged(rig, other);
    CHECK(rig.coordinator->prepared_count() == 1); rig.Idle();
    const auto view = rig.coordinator->GetPreparedJob(request.owner, registered.facts->job_id);
    REQUIRE(view.has_value()); CHECK(view->facts == registered.facts);
    REQUIRE(rig.coordinator->Shutdown()); rig.coordinator.reset();
    // A new actual coordinator cannot rediscover the source as a new ticket.
    ToolJobCoordinator::Options options; options.prepared_registration = PreparedRegistrationContext{rig.serial, "project-fixture", rig.root.path};
    rig.coordinator = std::make_shared<ToolJobCoordinator>(*rig.writer, nullptr, nullptr, options);
    const auto owner = rig.coordinator->PreparedOwner(); REQUIRE(owner.has_value()); request.owner = *owner;
    RejectUnchanged(rig, request); CHECK(rig.coordinator->prepared_count() == 0);
    Mark("capacity");
}

TEST_CASE("Owned Job registration preserves native rejection and partial write uncertainty without retry") {
    for (unsigned failed_write = 1; failed_write != 4; ++failed_write) {
        Rig rig("receipts-" + std::to_string(failed_write), 1);
        const auto source = rig.Declare(); auto request = rig.Prepare(source);
        const auto before = Read(rig.journal); rig.fault->writes = 0;
        if (failed_write < 3) rig.fault->fail_at = failed_write;
        else request.policy.resource_keys = {""}; // The real Registered schema rejects; Pending has already committed.
        const auto result = rig.coordinator->RegisterPreparedJob(request);
        REQUIRE_MESSAGE(result.state == PreparedJobRegistrationState::Unconfirmed, result.error); REQUIRE(result.facts);
        REQUIRE(result.facts->pending_receipt.has_value()); CHECK(rig.writer->broken() == (failed_write < 3));
        const auto& failed = failed_write == 1 ? result.facts->pending_receipt : result.facts->registered_receipt;
        REQUIRE(failed.has_value());
        // First two variants inject at the actual native append boundary. The
        // third is a healthy schema rejection after a real committed Pending.
        // Keep the actual Rejected enum; none simulates a natural OS fsync error.
        CHECK(failed->status == v3::WriteReceipt::Status::Rejected); CHECK_FALSE(failed->error_code.empty());
        CHECK(failed->id.empty()); CHECK_FALSE(result.error_code.empty());
        const auto actual = Read(rig.journal);
        CHECK(actual.events.size() == before.events.size() + (failed_write > 1 ? 1 : 0));
        if (failed_write > 1) ReceiptInLedger(actual, *result.facts->pending_receipt, v3::EventKindV3::ToolExecutionPending);
        CHECK(rig.coordinator->prepared_count() == 1); rig.Idle();
        const auto bytes = Bytes(rig.journal);
        const auto view = rig.coordinator->GetPreparedJob(request.owner, result.facts->job_id);
        REQUIRE(view.has_value()); CHECK(view->state == PreparedJobRegistrationState::Unconfirmed); CHECK(view->facts == result.facts);
        const auto repeat = rig.coordinator->RegisterPreparedJob(request);
        CHECK(repeat.state != PreparedJobRegistrationState::Registered); CHECK(Bytes(rig.journal) == bytes);
        // A real AppendEvent on the now-broken writer supplies IoFailed. The
        // registration preflight must not invent that receipt for a new call.
        if (failed_write < 3) {
            v3::EventDraft draft; draft.kind = v3::EventKindV3::ToolJobRegistered; draft.payload = Json::object();
            const auto native = rig.writer->AppendEvent(std::move(draft), v3::Durability::PowerLoss);
            CHECK(native.status == v3::WriteReceipt::Status::IoFailed); CHECK(native.id.empty()); CHECK_FALSE(native.error_code.empty());
        } else {
            const auto other = rig.Declare(2); auto next = rig.Prepare(other);
            const auto prior = Bytes(rig.journal);
            const auto capacity = rig.coordinator->RegisterPreparedJob(next);
            CHECK(capacity.state == PreparedJobRegistrationState::Rejected); CHECK(capacity.error_code == "job.prepared.capacity");
            CHECK(Bytes(rig.journal) == prior); CHECK(rig.coordinator->prepared_count() == 1);
            CHECK(rig.coordinator->PumpCompletions() == 0); rig.Idle();
            continue;
        }
        CHECK(Bytes(rig.journal) == bytes); CHECK(rig.coordinator->PumpCompletions() == 0); rig.Idle();
    }
    Mark("receipts");
}

TEST_CASE("Owned Job registration close and default recovery cannot promote a temporary ticket") {
    {
        Rig closed("closed-first"); const auto source = closed.Declare(); const auto other = closed.Declare(2);
        auto request = closed.Prepare(source); const auto different = closed.Prepare(other);
        REQUIRE(closed.writer->Close().has_value()); REQUIRE(closed.writer->closed());
        RejectUnchanged(closed, request, "job.prepared.closed"); RejectUnchanged(closed, different, "job.prepared.closed");
        CHECK_FALSE(closed.coordinator->PreparedOwner()); CHECK(closed.coordinator->prepared_count() == 0);
    }
    Rig rig("close-isolation"); const auto source = rig.Declare(); auto request = rig.Prepare(source);
    const auto result = rig.coordinator->RegisterPreparedJob(request); REQUIRE(result.facts);
    REQUIRE(result.state == PreparedJobRegistrationState::Registered);
    const auto before = Bytes(rig.journal); const auto& job = result.facts->job_id;
    JobStartRequest legacy;
    legacy.tool_name = request.tool_name; legacy.tool_input = request.original_input;
    legacy.turn_id = request.turn_id; legacy.step_id = request.step_id; legacy.assistant_message_ref = source.message;
    CHECK_FALSE(rig.coordinator->StartJob(legacy).ok); CHECK_FALSE(rig.coordinator->StartJobEarly(legacy).ok);
    CHECK(rig.coordinator->PumpCompletions() == 0); CHECK_FALSE(rig.coordinator->GrantApproval(job).ok);
    CHECK_FALSE(rig.coordinator->CompleteAdmission(job)); CHECK(rig.coordinator->GetJob(job).state.empty());
    const auto waited = rig.coordinator->WaitJobs({job}, 1, true); CHECK_FALSE(waited.satisfied);
    rig.coordinator->CancelJob(job, "not-a-business-job");
    CHECK_FALSE(rig.coordinator->DebugSubmitEnvelope(job, "foreign-epoch", {"late", false}));
    CHECK(rig.coordinator->AdoptRecovery(ToolJobCoordinator::PlanRecovery(Read(rig.journal))) == 0);
    CHECK(Bytes(rig.journal) == before); rig.Idle();
    ShutdownUnderSerial(rig);
    const auto revoked = rig.coordinator->GetPreparedJob(request.owner, job); REQUIRE(revoked.has_value());
    CHECK(revoked->revoked); CHECK(revoked->facts == result.facts); RejectUnchanged(rig, request);
    rig.coordinator.reset(); REQUIRE(rig.writer->Close().has_value()); rig.writer.reset();
    const auto persisted = Read(rig.journal); StagedOnly(persisted, *result.facts);
    const auto plan = ToolJobCoordinator::PlanRecovery(persisted);
    REQUIRE(plan.policy == JobRecoveryPolicy::Legacy); REQUIRE(plan.items.size() == 1);
    CHECK(plan.items.front().disposition == "prepared_hold");
    auto resumed = v3::V3Writer::Continue(rig.journal); REQUIRE(resumed.has_value()); rig.writer = std::move(*resumed);
    // This fresh default/Legacy instance has a real executor and no new-domain
    // guard. The persisted preparedOnly marker itself must block replay.
    rig.coordinator = std::make_shared<ToolJobCoordinator>(*rig.writer,
        [counts = rig.counts](const std::string&, const Json&) { ++counts->gate; return JobAuthDecision{true, false, {}}; },
        [counts = rig.counts](const JobExecutionContext&) { ++counts->execute; return Tool::Result{"replayed", false}; });
    const auto continued = Bytes(rig.journal);
    CHECK(rig.coordinator->AdoptRecovery(plan) == 0); CHECK(rig.coordinator->PumpCompletions() == 0);
    const auto held_plan = ToolJobCoordinator::PlanRecovery(persisted, JobRecoveryPolicy::Hold);
    REQUIRE(held_plan.items.size() == 1); CHECK(held_plan.items.front().disposition == "prepared_hold");
    CHECK(rig.coordinator->AdoptRecovery(held_plan) == 0);
    CHECK(rig.coordinator->GetJob(job).state == "unknown_job"); CHECK_FALSE(rig.coordinator->GrantApproval(job).ok);
    CHECK_FALSE(rig.coordinator->CompleteAdmission(job)); REQUIRE(rig.coordinator->Shutdown());
    CHECK(Bytes(rig.journal) == continued); CHECK(rig.counts->execute.load() == 0);
    CHECK(rig.coordinator->running_count() == 0); CHECK(rig.coordinator->queued_count() == 0);
    {
        Rig legacy_rig("legacy-control"); const auto declaration = legacy_rig.Declare();
        REQUIRE(legacy_rig.coordinator->Shutdown()); legacy_rig.coordinator.reset();
        legacy_rig.coordinator = std::make_shared<ToolJobCoordinator>(*legacy_rig.writer,
            [](const std::string&, const Json&) { return JobAuthDecision{true, false, {}}; },
            [counts = legacy_rig.counts](const JobExecutionContext&) { ++counts->execute; return Tool::Result{"ordinary-result", false}; });
        JobStartRequest ordinary;
        ordinary.tool_name = declaration.call.name; ordinary.tool_input = declaration.call.input;
        ordinary.turn_id = "turn-000001"; ordinary.step_id = "step-000001"; ordinary.assistant_message_ref = declaration.message;
        ordinary.policy.allow_background = true;
        const auto started = legacy_rig.coordinator->StartJob(ordinary); REQUIRE_MESSAGE(started.ok, started.error);
        const auto wait = legacy_rig.coordinator->WaitJobs({started.job_id}, 5000, true);
        REQUIRE(wait.satisfied); REQUIRE(wait.statuses.size() == 1); CHECK(wait.statuses.front().state == "succeeded");
        REQUIRE(legacy_rig.coordinator->Shutdown()); CHECK(legacy_rig.counts->execute.load() == 1);
        CHECK_FALSE(legacy_rig.coordinator->PreparedOwner());
    }
    Mark("close-isolation");
}
