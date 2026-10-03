#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include "agent/agent.hpp"
#include "platform/sha256.hpp"
#include "runtime/assembly/session_resources.hpp"
#include "runtime/scoped_turn_bindings.hpp"
#include "runtime/session_service.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "runtime/trajectory_session.hpp"
#include "sdk/operation_ledger.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"
#include "workspace/identity.hpp"

namespace {
namespace fs = std::filesystem;
namespace sdk = lubancore::detail;
namespace rt = lubancode::runtime;
namespace api = lubancode::api;
namespace tools = lubancode::tools;
namespace v3 = lubancode::trajectory::v3;
namespace agent = lubancode::agent;
using Json = nlohmann::json;
using namespace std::chrono_literals;
using Knowledge = sdk::OperationTurnKnowledge;
using Material = sdk::OperationTurnMaterialState;

struct Directory {
    fs::path root;
    Directory() {
        try {
            static std::atomic<unsigned> serial{0};
            for (unsigned attempt = 0; attempt < 64; ++attempt) {
                auto candidate = fs::temp_directory_path() / ("sdk-operation-turn-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                    std::to_string(++serial));
                if (fs::create_directory(candidate)) { root = std::move(candidate); root = fs::canonical(root); break; }
            }
            REQUIRE_FALSE(root.empty());
            REQUIRE(fs::create_directory(root / "a"));
            REQUIRE(fs::create_directory(root / "b"));
        } catch (...) {
            if (!root.empty()) { std::error_code ec; fs::remove_all(root, ec); }
            throw;
        }
    }
    ~Directory() { std::error_code ec; fs::remove_all(root, ec); }
};
std::string Bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    std::string bytes((std::istreambuf_iterator<char>(stream)), {});
    REQUIRE_FALSE(stream.bad());
    return bytes;
}
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.is_open()); stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close(); REQUIRE_FALSE(stream.fail());
}
std::vector<Json> Rows(const std::string& bytes) {
    std::vector<Json> rows;
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto end = bytes.find('\n', offset); REQUIRE(end != std::string::npos);
        rows.push_back(Json::parse(bytes.substr(offset, end - offset))); offset = end + 1;
    }
    return rows;
}
std::string DumpRows(const std::vector<Json>& rows, bool rehash = false) {
    std::string bytes, previous(v3::kGenesisHash);
    for (auto row : rows) {
        if (rehash) {
            if (row.value("kind", std::string()) == "model.request.prepared") {
                row["payload"]["readThroughHash"] = previous;
                row["payload"]["readThroughSeq"] = row.at("seq").get<std::uint64_t>() - 1;
            }
            row.erase("prevHash"); row.erase("lineHash");
            const auto payload = lubancode::trajectory::CanonicalJsonDump(row);
            REQUIRE(payload.has_value());
            const auto hash = v3::ComputeLineHash(previous, *payload);
            row["prevHash"] = previous; row["lineHash"] = hash; previous = hash;
        }
        const auto text = lubancode::trajectory::CanonicalJsonDump(row);
        REQUIRE(text.has_value()); bytes += *text + "\n";
    }
    return bytes;
}
v3::V3Ledger Ledger(const fs::path& path) {
    auto read = v3::ReadV3Ledger(path);
    REQUIRE_MESSAGE(read.has_value(), (read ? std::string() : read.error()));
    return std::move(*read);
}
void Committed(const v3::WriteReceipt& receipt) {
    REQUIRE_MESSAGE(receipt.status == v3::WriteReceipt::Status::Committed, receipt.error_message);
    REQUIRE_FALSE(receipt.id.empty()); REQUIRE(receipt.seq > 0); REQUIRE(v3::IsHex64(receipt.line_hash));
}
void NativeFacts(const v3::V3Ledger& ledger, const v3::OperationTurnBindingFacts& facts) {
    const auto* event = ledger.FindEvent(facts.event_id); REQUIRE(event != nullptr);
    CHECK(event->kind == v3::EventKindV3::SdkOperationTurnBound);
    CHECK(event->session_id == facts.session_id); CHECK(event->run_id == facts.run_id);
    CHECK(event->turn_id == facts.turn_id); CHECK(event->seq == facts.seq); CHECK(event->line_hash == facts.line_hash);
    CHECK(event->payload.at("operationId").get<std::string>() == facts.operation_id);
    CHECK(event->payload.at("inputId").get<std::string>() == facts.input_id);
    CHECK(event->payload.at("payloadHash").get<std::string>() == facts.payload_hash);
    CHECK_FALSE(event->status.has_value());
}
struct Gate {
    std::mutex mutex; std::condition_variable cv; unsigned arrived = 0; bool release = false;
    bool Enter() {
        std::unique_lock lock(mutex); ++arrived; cv.notify_all();
        return cv.wait_for(lock, 20s, [&] { return release; });
    }
    void Release() { std::lock_guard lock(mutex); release = true; cv.notify_all(); }
};
struct State {
    std::atomic<unsigned> models{0}, tools{0};
    std::mutex mutex; std::vector<api::Request> requests;
    std::shared_ptr<Gate> gate;
    std::string name;
};
class Backend final : public api::Backend {
public:
    explicit Backend(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::expected<void, api::Error> send_stream(const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& emit, const std::atomic<bool>*) override {
        const auto call = state_->models.fetch_add(1);
        { std::lock_guard lock(state_->mutex); state_->requests.push_back(request); }
        if (call == 0 && state_->gate && !state_->gate->Enter())
            return std::unexpected(api::Error{api::ErrorKind::Parse, "fixture gate did not release"});
        emit(api::MessageStart{"binding-response", request.model});
        if (call % 2 == 0) {
            emit(api::ToolUseStart{0, "provider-binding-call", "binding_echo"});
            emit(api::ToolUseInputDelta{0, "{}"}); emit(api::ContentBlockDone{0});
            emit(api::MessageDone{"tool_use", api::Usage{5, 3, 0, 0, 0}});
        } else {
            emit(api::TextDelta{"answer-" + state_->name}); emit(api::ContentBlockDone{0});
            emit(api::MessageDone{"end_turn", api::Usage{5, 3, 0, 0, 0}});
        }
        return {};
    }
private:
    std::shared_ptr<State> state_;
};
class Echo final : public tools::Tool {
public:
    explicit Echo(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::string name() const override { return "binding_echo"; }
    std::string description() const override { return "Return the current scene's owned text."; }
    Json input_schema() const override { return {{"type", "object"}, {"properties", Json::object()}}; }
    tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }
    Result execute(const Json&) override { ++state_->tools; return Result::Text("tool-" + state_->name); }
private:
    std::shared_ptr<State> state_;
};
struct Rig {
    std::shared_ptr<State> state;
    std::unique_ptr<rt::SessionService> service;
    explicit Rig(const Directory& directory, std::string name = "actual", fs::path cwd = {},
                 std::shared_ptr<Gate> gate = {}) : state(std::make_shared<State>()) {
        state->name = std::move(name); state->gate = std::move(gate);
        if (cwd.empty()) cwd = directory.root / "a";
        rt::SessionLaunchRequest launch;
        launch.cwd_utf8 = tools::PathToUtf8(cwd);
        launch.workspace_identity = lubancode::workspace::MakeFallbackIdentity(cwd);
        launch.workspaces_root = directory.root / "data"; launch.lubancode_version = "binding-fixture";
        launch.wire_name = "chat"; launch.v3_system_content = "binding system";
        service = std::make_unique<rt::SessionService>(std::move(launch));
        REQUIRE_MESSAGE(service->runtime() != nullptr, service->launch_error()); REQUIRE(service->v3_format());
        rt::assembly::SessionResourcesRequest request;
        request.backend_factory = [owned = state] { return std::make_unique<Backend>(owned); };
        request.registry_factory = [owned = state](std::span<const rt::assembly::McpServerRuntime>)
            -> rt::assembly::SessionRegistryResult {
            auto registry = std::make_unique<tools::ToolRegistry>(); registry->Register(std::make_unique<Echo>(owned));
            return registry;
        };
        auto resources = rt::assembly::BuildSessionResources(std::move(request)); REQUIRE(resources.has_value());
        agent::AgentProfile profile; profile.provider = "fixture"; profile.request.model = "binding-model";
        profile.system_prompt = "binding system"; profile.runtime.max_steps_per_turn = 4;
        profile.runtime.max_output_tokens = 1024;
        service->InitializeExecution(std::move(*resources), std::move(profile));
    }
    ~Rig() { if (service) { service->ShutdownExecution(); service->Close("fixture_exit"); } }
    v3::V3Writer& Writer() { return *service->trajectory()->v3_main_writer(); }
    fs::path Journal() { return Writer().path(); }
    fs::path Dir() { return service->trajectory()->session_dir(); }
    rt::SessionService::InputReceipt Submit(std::string key = "input-key", std::string text = "real input") {
        auto accepted = service->SubmitInput({std::move(key), std::move(text), {}});
        REQUIRE_MESSAGE(accepted.accepted, accepted.error_code); return accepted;
    }
    void Run(const sdk::MainOperationTurnStart& start) {
        REQUIRE(start.ready()); REQUIRE(start.input.has_value()); REQUIRE(start.facts.has_value());
        auto bridge = service->trajectory()->NewTurnBridge({"fixture", "chat", "binding-model", {}});
        REQUIRE(bridge != nullptr);
        rt::ToolTraceHub hub(service->runtime()->ids()); agent::TurnWiring wiring; wiring.turn_id = start.turn_id;
        rt::ScopedTurnBindings bindings(service->execution()->agent());
        bindings.Bind(wiring, {&hub, bridge.get(), nullptr, Writer().session_id(), start.turn_id, std::nullopt});
        bridge->BeginTurn(start.turn_id, "external_user");
        api::Message input{api::Role::User, {api::TextBlock{start.input->text}}}; bridge->RecordInput(input);
        const auto run = service->execution()->agent().Run(std::move(input), wiring);
        REQUIRE_MESSAGE(run.has_value(), (run ? std::string() : run.error()));
        REQUIRE_FALSE(run->cancelled); REQUIRE_FALSE(run->side_effect_indeterminate);
        bridge->EndTurn(true, false, {}); bindings.Reset();
        REQUIRE_FALSE(bridge->last_committed_assistant_message_id().empty());
        REQUIRE(service->RecordTurnFinal({start.input->operation_id, start.turn_id, "success",
            {bridge->last_committed_assistant_message_id()}, true}));
    }
    void NoExecution() const { CHECK(state->models.load() == 0); CHECK(state->tools.load() == 0); }
};
void Marker(const char* path) { std::cout << "[sdk-operation-turn-binding-path] " << path << std::endl; }
} // namespace

TEST_CASE("SDK actual popped operation binds native turn before model and tool") {
    Directory directory; Rig rig(directory);
    const auto first = rig.Submit(); const auto second = rig.Submit("second", "second input");
    unsigned publications = 0;
    const auto start = sdk::BeginMainOperationTurn(*rig.service, [&](const auto& actual) {
        ++publications; CHECK(actual.operation_id == first.operation_id); CHECK(actual.input_id == first.input_id);
        CHECK(actual.payload_hash == first.payload_hash); rig.NoExecution();
    });
    REQUIRE(start.ready()); REQUIRE(start.receipt); REQUIRE(start.facts); Committed(*start.receipt);
    CHECK(publications == 1); CHECK(rig.service->pending_input_count() == 1);
    CHECK(start.input->text == "real input"); CHECK(start.input->operation_id == first.operation_id);
    NativeFacts(Ledger(rig.Journal()), *start.facts); rig.Run(start);
    CHECK(rig.state->models.load() == 2); CHECK(rig.state->tools.load() == 1);
    const auto next = sdk::BeginMainOperationTurn(*rig.service); REQUIRE(next.ready()); REQUIRE(next.facts);
    CHECK(next.facts->operation_id == second.operation_id); CHECK(next.turn_id != start.turn_id);
    CHECK(rig.service->pending_input_count() == 0); rig.Run(next);
    const auto ledger = Ledger(rig.Journal()); const auto checked = sdk::CheckMainOperationTurnBindings(rig.Dir(), ledger);
    REQUIRE(checked.state == Material::Validated); REQUIRE(checked.facts.size() == 2);
    NativeFacts(ledger, checked.facts[0]); NativeFacts(ledger, checked.facts[1]);
    const auto operations = rt::SessionService::ReadOperationFactsOwned(Bytes(rig.Dir() / "operations.jsonl"));
    REQUIRE(operations); CHECK(operations->size() == 6);
    CHECK(std::count_if(operations->begin(), operations->end(), [](const auto& fact) {
        return fact.kind == "operation.dispatched";
    }) == 2);
    CHECK(rig.state->models.load() == 4); CHECK(rig.state->tools.load() == 2); Marker("actual");
}

TEST_CASE("SDK operation turn missing or mismatched source stops after the actual Pop") {
    for (int variant = 0; variant < 4; ++variant) {
        Directory directory; Rig rig(directory); const auto journal = Bytes(rig.Journal());
        if (variant == 0) {
            const auto empty = sdk::BeginMainOperationTurn(*rig.service);
            CHECK(empty.knowledge == Knowledge::NotConsumed); CHECK(empty.error.code == "sdk.operation_turn.empty");
            CHECK_FALSE(empty.receipt); CHECK_FALSE(empty.input);
        } else {
            rig.Submit();
            if (variant == 3) {
                REQUIRE(rig.Writer().Close().has_value());
            } else {
                const auto path = rig.Dir() / "operations.jsonl";
                auto rows = Rows(Bytes(path)); REQUIRE(rows.size() == 1);
                if (variant == 1) rows[0]["payloadHash"] = std::string(64, 'f');
                else rows.clear();
                Write(path, DumpRows(rows));
            }
            const auto rejected = sdk::BeginMainOperationTurn(*rig.service);
            CHECK_FALSE(rejected.ready()); CHECK_FALSE(rejected.receipt); CHECK_FALSE(rejected.facts);
            CHECK(rejected.turn_id.empty());
            if (variant == 3) {
                CHECK(rejected.knowledge == Knowledge::NotConsumed); CHECK(rig.service->pending_input_count() == 1);
                CHECK(rejected.error.code == "sdk.operation_turn.writer_unavailable");
            } else {
                CHECK(rejected.knowledge == Knowledge::ConsumedNoAnchor); CHECK(rejected.input.has_value());
                CHECK(rig.service->pending_input_count() == 0);
                CHECK(rejected.error.code == (variant == 1 ? "sdk.operation_turn.invalid_source" : "sdk.resume.operation_ledger_invalid"));
            }
        }
        CHECK(Bytes(rig.Journal()) == journal); rig.NoExecution();
    }
    Marker("source");
}

TEST_CASE("SDK operation turn preserves first native uncertainty and publication gap") {
    for (int fault = 0; fault < 2; ++fault) {
        Directory directory; Rig rig(directory); rig.Submit(); const auto path = rig.Journal();
        if (fault == 0) {
            // Reopen the actual Service writer with its existing internal I/O
            // seam. This injects the real append boundary, not a fake receipt.
            REQUIRE(rig.Writer().Close().has_value());
            v3::V3WriterOptions options;
            options.inject_io_failure = [] { return std::optional<std::string>("fixture anchor append"); };
            auto reopened = v3::V3Writer::Continue(path, std::move(options)); REQUIRE(reopened.has_value());
            rig.Writer() = std::move(*reopened);
        }
        unsigned publications = 0;
        const auto start = sdk::BeginMainOperationTurn(*rig.service, [&](const auto&) {
            ++publications; throw std::runtime_error("actual owned publication rejected");
        });
        REQUIRE_FALSE(start.ready()); REQUIRE(start.input); REQUIRE(start.receipt); CHECK_FALSE(start.turn_id.empty());
        CHECK(rig.service->pending_input_count() == 0);
        if (fault == 0) {
            CHECK(start.knowledge == Knowledge::AppendUnconfirmed);
            CHECK(start.receipt->status == v3::WriteReceipt::Status::Rejected);
            CHECK(start.receipt->error_code == "v3writer.injected"); CHECK(rig.Writer().broken());
            CHECK(start.error.code == start.receipt->error_code); CHECK_FALSE(start.facts); CHECK(publications == 0);
            const auto anchors = v3::ReadOperationTurnBindings(Ledger(path)); REQUIRE(anchors); CHECK(anchors->empty());
        } else {
            CHECK(start.knowledge == Knowledge::CommittedPublicationGap); Committed(*start.receipt);
            REQUIRE(start.facts); NativeFacts(Ledger(path), *start.facts); CHECK(publications == 1);
            CHECK(start.error.message == "actual owned publication rejected");
            const auto material = sdk::CheckMainOperationTurnBindings(rig.Dir(), Ledger(path));
            CHECK(material.state == Material::Incomplete); REQUIRE(material.facts.size() == 1);
        }
        const auto before = Bytes(path); const auto next = sdk::BeginMainOperationTurn(*rig.service);
        CHECK_FALSE(next.ready()); CHECK_FALSE(next.receipt); CHECK_FALSE(next.input);
        CHECK(Bytes(path) == before); rig.NoExecution();
    }
    Marker("gap");
}

TEST_CASE("SDK operation turn historical prefixes remain passive and old journals unbound") {
    for (int window = 0; window < 3; ++window) {
        Directory directory; Rig rig(directory); rig.Submit();
        if (window == 0) {
            // Real legacy producer path: Pop has committed dispatch but never
            // called the new optional seam. No anchor is guessed from that row.
            const auto popped = rig.service->PopPendingInput();
            REQUIRE(popped.status == rt::SessionService::PendingPop::Status::Ok);
            CHECK(rig.service->pending_input_count() == 0);
        } else {
            const auto start = sdk::BeginMainOperationTurn(*rig.service); REQUIRE(start.ready());
            if (window == 2) rig.Run(start);
        }
        const auto path = rig.Journal(), dir = rig.Dir();
        REQUIRE(rig.service->Close("historical_binding").error_code.empty());
        const auto before = Bytes(path), operations_before = Bytes(dir / "operations.jsonl");
        const auto material = sdk::CheckMainOperationTurnBindings(dir, Ledger(path));
        CHECK(material.state == (window == 0 ? Material::NotApplicable :
                                 window == 1 ? Material::Incomplete : Material::Validated));
        auto continued = v3::V3Writer::Continue(path); REQUIRE(continued.has_value());
        CHECK(continued->session_id() == rig.Writer().session_id());
        CHECK(continued->run_id() == rig.Writer().run_id()); REQUIRE(continued->Close().has_value());
        const auto reread = sdk::CheckMainOperationTurnBindings(dir, Ledger(path)); CHECK(reread.state == material.state);
        CHECK(Bytes(path) == before); CHECK(Bytes(dir / "operations.jsonl") == operations_before);
        if (window != 2) rig.NoExecution();
        else { CHECK(rig.state->models.load() == 2); CHECK(rig.state->tools.load() == 1); }
    }
    Marker("history");
}

TEST_CASE("SDK operation turn strict history rejects rehashed wrong relationships") {
    Directory directory; Rig rig(directory); const auto accepted = rig.Submit();
    const auto start = sdk::BeginMainOperationTurn(*rig.service); REQUIRE(start.ready()); REQUIRE(start.facts);
    rig.Run(start); const auto path = rig.Journal(), dir = rig.Dir();
    REQUIRE(rig.service->Close("relation_fixture").error_code.empty());
    const auto original = Bytes(path), original_operations = Bytes(dir / "operations.jsonl");
    for (int variant = 0; variant < 6; ++variant) {
        auto rows = Rows(original); bool changed = false;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            auto& row = rows[i];
            if (row.value("kind", std::string()) != "sdk.operation.turn.bound") continue;
            if (variant == 0) row["payload"]["inputId"] = "foreign-input";
            if (variant == 1) row["payload"]["operationId"] = "foreign-operation";
            if (variant == 2) row["payload"]["payloadHash"] = std::string(64, 'f');
            if (variant == 3) {
                // A duplicate real anchor gets its own well-shaped event ID.
                auto duplicate = row; duplicate["eventId"] = "evt-extra-binding";
                rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(i + 1), std::move(duplicate));
            }
            if (variant == 5) {
                // Move the existing real anchor behind an actual same-turn
                // input fact. Hash/shape stay valid; chronology must refuse it.
                REQUIRE(i + 1 < rows.size());
                REQUIRE(rows[i + 1].at("turnId").get<std::string>() == start.turn_id);
                std::swap(rows[i], rows[i + 1]);
            }
            changed = true; break;
        }
        REQUIRE(changed);
        if (variant == 4) {
            auto operations = Rows(original_operations);
            for (auto& row : operations) if (row.at("kind") == "operation.final") row["turnId"] = "foreign-turn";
            Write(dir / "operations.jsonl", DumpRows(operations));
        } else {
            for (std::size_t i = 0; i < rows.size(); ++i) rows[i]["seq"] = i + 1;
            Write(path, DumpRows(rows, true));
        }
        const auto verified = v3::ReadV3Ledger(path);
        if (variant == 3 || variant == 5) {
            REQUIRE_FALSE(verified); CHECK(verified.error().find("v3reader.operation_turn_invalid") != std::string::npos);
        } else {
            // Shape, native hash chain and ordinary context replay must pass
            // before the strict operations relationship is asked to reject.
            REQUIRE_MESSAGE(verified.has_value(), (verified ? std::string() : verified.error()));
            const auto rejected = sdk::CheckMainOperationTurnBindings(dir, *verified);
            CHECK(rejected.state == Material::Rejected); CHECK(rejected.error.code == "sdk.resume.operation_turn_invalid");
        }
        CHECK(rig.state->models.load() == 2); CHECK(rig.state->tools.load() == 1);
        Write(path, original); Write(dir / "operations.jsonl", original_operations);
    }
    for (int owner_variant = 0; owner_variant < 5; ++owner_variant) {
        CAPTURE(owner_variant);
        auto rows = Rows(original);
        const auto own_turn = [&](const Json& row) {
            const auto turn = row.find("turnId");
            return turn != row.end() && turn->is_string() && turn->get<std::string>() == start.turn_id;
        };
        if (owner_variant == 0) {
            const auto started = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
                return row.value("kind", std::string()) == "session.started";
            });
            REQUIRE(started != rows.end());
            REQUIRE(started->at("sessionId").get<std::string>() == start.facts->session_id);
            REQUIRE(started->at("runId").get<std::string>() == start.facts->run_id);
            REQUIRE(started->at("seq").get<std::uint64_t>() < start.facts->seq);
            (*started)["sessionId"] = "foreign-start-session";
            (*started)["runId"] = "foreign-start-run";
            // Keep every anchored-turn row on that run. The old run-only
            // lookup could pair this local-session anchor with a foreign SID's
            // Started; neither the original input nor its context is changed.
            unsigned changed = 0;
            for (auto& row : rows) if (own_turn(row)) {
                REQUIRE(row.at("sessionId").get<std::string>() == start.facts->session_id);
                row["runId"] = "foreign-start-run";
                ++changed;
            }
            REQUIRE(changed > 1);
        } else {
            const bool message = owner_variant <= 2;
            const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& candidate) {
                if (!own_turn(candidate)) return false;
                return message ? candidate.at("type") == "message"
                               : candidate.value("kind", std::string()) == "input.received";
            });
            REQUIRE(row != rows.end());
            REQUIRE(row->at("sessionId").get<std::string>() == start.facts->session_id);
            REQUIRE(row->at("runId").get<std::string>() == start.facts->run_id);
            if (owner_variant == 1 || owner_variant == 3) (*row)["sessionId"] = "foreign-turn-session";
            else (*row)["runId"] = "foreign-turn-run";
        }
        const auto changed = DumpRows(rows, true);
        Write(path, changed);
        // Native verification proves the modified real journal still has a
        // legal shape, hash chain and context replay before the Reader rejects
        // the cross-row owner relationship.
        const auto shape = v3::VerifyV3File(path);
        const auto shape_error = shape.error_code + " " + shape.message;
        REQUIRE_MESSAGE(shape.ok, shape_error);
        const auto rejected = v3::ReadV3Ledger(path);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().find("v3reader.operation_turn_invalid") != std::string::npos);
        CHECK(Bytes(path) == changed);
        CHECK(Bytes(dir / "operations.jsonl") == original_operations);
        CHECK(rig.state->models.load() == 2); CHECK(rig.state->tools.load() == 1);
        Write(path, original);
    }
    {
        // A legitimate later run is paired with its own local Started. The
        // first system-message run is not an authority for subsequent anchors.
        auto rows = Rows(original);
        const auto anchor = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
            return row.value("kind", std::string()) == "sdk.operation.turn.bound";
        });
        REQUIRE(anchor != rows.end());
        rows.erase(anchor + 1, rows.end());
        for (auto& row : rows) if (row.value("kind", std::string()) == "session.started" ||
                                  row.value("kind", std::string()) == "sdk.operation.turn.bound")
            row["runId"] = "legitimate-later-run";
        Write(path, DumpRows(rows, true));
        const auto read = Ledger(path);
        const auto bound = v3::ReadOperationTurnBindings(read);
        REQUIRE(bound); REQUIRE(bound->size() == 1);
        CHECK(bound->front().session_id == read.session_id);
        CHECK(bound->front().run_id == "legitimate-later-run");
        CHECK(bound->front().run_id != read.run_id);
        CHECK(Bytes(dir / "operations.jsonl") == original_operations);
        CHECK(rig.state->models.load() == 2); CHECK(rig.state->tools.load() == 1);
        Write(path, original);
    }
    const auto ledger = Ledger(path); const auto checked = sdk::CheckMainOperationTurnBindings(dir, ledger);
    REQUIRE(checked.state == Material::Validated); REQUIRE(checked.facts.size() == 1);
    CHECK(checked.facts[0].operation_id == accepted.operation_id); NativeFacts(ledger, checked.facts[0]);
    Marker("relation");
}

TEST_CASE("SDK operation turn four real sessions keep native owners and inputs isolated") {
    Directory directory; auto gate = std::make_shared<Gate>();
    std::array<std::unique_ptr<Rig>, 4> rigs;
    std::array<rt::SessionService::InputReceipt, 4> accepted;
    for (std::size_t i = 0; i < rigs.size(); ++i) {
        rigs[i] = std::make_unique<Rig>(directory, "scene-" + std::to_string(i),
            directory.root / (i < 2 ? "a" : "b"), gate);
        accepted[i] = rigs[i]->Submit("same-client-key", "input-scene-" + std::to_string(i));
    }
    std::array<std::future<sdk::MainOperationTurnStart>, 4> futures;
    struct Finish {
        std::shared_ptr<Gate> gate; std::array<std::future<sdk::MainOperationTurnStart>, 4>& futures;
        ~Finish() { gate->Release(); for (auto& future : futures) if (future.valid()) future.wait(); }
    } finish{gate, futures};
    for (std::size_t i = 0; i < rigs.size(); ++i) {
        futures[i] = std::async(std::launch::async, [&, i] {
            auto start = sdk::BeginMainOperationTurn(*rigs[i]->service); rigs[i]->Run(start); return start;
        });
    }
    {
        std::unique_lock lock(gate->mutex); REQUIRE(gate->cv.wait_for(lock, 5s, [&] { return gate->arrived == 4; }));
    }
    for (auto& future : futures) CHECK(future.wait_for(0ms) == std::future_status::timeout);
    gate->Release();
    std::array<sdk::MainOperationTurnStart, 4> starts;
    for (std::size_t i = 0; i < rigs.size(); ++i) {
        REQUIRE(futures[i].wait_for(20s) == std::future_status::ready); starts[i] = futures[i].get();
        const auto ledger = Ledger(rigs[i]->Journal());
        const auto checked = sdk::CheckMainOperationTurnBindings(rigs[i]->Dir(), ledger);
        REQUIRE(checked.state == Material::Validated); REQUIRE(checked.facts.size() == 1); REQUIRE(starts[i].facts);
        NativeFacts(ledger, *starts[i].facts);
        CHECK(checked.facts[0].session_id == rigs[i]->Writer().session_id());
        CHECK(checked.facts[0].run_id == rigs[i]->Writer().run_id());
        CHECK(checked.facts[0].operation_id == accepted[i].operation_id);
        CHECK(checked.facts[0].input_id == accepted[i].input_id); CHECK(checked.facts[0].payload_hash == accepted[i].payload_hash);
        CHECK(rigs[i]->state->models.load() == 2); CHECK(rigs[i]->state->tools.load() == 1);
        std::lock_guard lock(rigs[i]->state->mutex);
        REQUIRE(rigs[i]->state->requests.size() == 2);
        for (const auto& request : rigs[i]->state->requests) {
            bool own_input = false;
            for (const auto& message : request.messages) for (const auto& block : message.content) {
                if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                    own_input = own_input || text->text == "input-scene-" + std::to_string(i);
                    for (std::size_t j = 0; j < rigs.size(); ++j) if (j != i)
                        CHECK(text->text != "input-scene-" + std::to_string(j));
                }
            }
            CHECK(own_input);
        }
        for (std::size_t j = 0; j < rigs.size(); ++j) if (i != j) {
            CHECK_FALSE(fs::equivalent(rigs[i]->Journal(), rigs[j]->Journal()));
            const auto foreign = sdk::CheckMainOperationTurnBindings(rigs[j]->Dir(), ledger);
            CHECK(foreign.state == Material::Rejected); CHECK(foreign.error.code == "sdk.resume.operation_turn_invalid");
        }
    }
    // IDs are session-scoped strings. Directory plus actual five-key values and
    // the accepted input hash establish ownership; no global-ID premise here.
    Marker("isolation");
}
