#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <lubancore/core.hpp>
#include <lubancore/memory.hpp>
#include <nlohmann/json.hpp>

#include "memory/project_memory.hpp"
#include "platform/sha256.hpp"
#include "runtime/memory_ledger_bridge.hpp"
#include "runtime/session_service.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"

namespace {
namespace sdk = lubancore;
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
using Json = nlohmann::json;
using namespace std::chrono_literals;
std::string Utf8(const fs::path& path) { return lubancode::tools::PathToUtf8(path); }
void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open()); out << text; out.close(); REQUIRE_FALSE(out.fail());
}
std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary); REQUIRE(in.is_open());
    std::string value{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(in.bad()); return value;
}
struct Fixture {
    fs::path root;
    Fixture() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("sdk-save-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        fs::create_directories(root / "repo"); fs::create_directories(root / "resources");
        Write(root / "repo" / "evidence.txt", "stable project evidence\n");
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    sdk::RuntimeOptions Roots() const { return {Utf8(root / "data"), Utf8(root / "resources")}; }
};
Json Input(const std::string& marker = "SDK_SAVE_OLD", const std::string& id = "preference.sdk-save") {
    return {{"kind", "preference"}, {"id", id}, {"title", "SDKSaveNeedle"},
        {"summary", "SDKSaveNeedle explicit project preference"}, {"content", "SDKSaveNeedle " + marker},
        {"keywords", Json::array({"SDKSaveNeedle"})}, {"confidence", "user-stated"}};
}
struct Trace {
    std::mutex mutex;
    std::atomic<unsigned> models{0};
    std::vector<Json> inputs{Input()};
    std::vector<sdk::ModelRequest> requests;
    bool pending = false;
    std::size_t turn = 0;
    bool prime = false, after = false;
};
class Backend final : public sdk::Backend {
public:
    explicit Backend(std::shared_ptr<Trace> trace) : trace_(std::move(trace)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancel) override {
        ++trace_->models;
        std::lock_guard lock(trace_->mutex); trace_->requests.push_back(request);
        if (cancel.requested()) return std::unexpected(sdk::Error{"fixture.cancelled", "cancelled"});
        if (trace_->turn >= trace_->inputs.size()) return sdk::ModelReply{"save-answer", {}, sdk::Usage{4, 3}};
        if (!trace_->pending) {
            trace_->pending = true;
            std::vector<sdk::ToolCall> calls;
            if (trace_->prime) calls.push_back({"prime", "prime", "{}"});
            calls.push_back({"same-provider-id", "memory_save", trace_->inputs[trace_->turn].dump()});
            if (trace_->after) calls.push_back({"after", "after", "{}"});
            return sdk::ModelReply{"", std::move(calls), sdk::Usage{4, 3}};
        }
        trace_->pending = false; ++trace_->turn;
        return sdk::ModelReply{"save-answer", {}, sdk::Usage{4, 3}};
    }
private:
    std::shared_ptr<Trace> trace_;
};
sdk::SessionOptions Options(const Fixture& fixture, const std::shared_ptr<Trace>& trace, bool enabled = true, fs::path cwd = {}) {
    sdk::SessionOptions options;
    options.cwd = Utf8(cwd.empty() ? fixture.root / "repo" : cwd);
    options.model = "memory-save-model"; options.system_prompt = "SAVE_HOST";
    options.backend = std::make_unique<Backend>(trace);
    if (enabled) options.memory_write = sdk::memory::v1::WriteOptions{};
    return options;
}
sdk::memory::v1::WriteSnapshot Snapshot(const std::shared_ptr<sdk::Session>& session) {
    auto value = session->DescribeMemoryWrite(); REQUIRE(value.has_value()); return *value;
}
fs::path Memory(const std::shared_ptr<sdk::Session>& session) { return lubancode::tools::Utf8ToPath(Snapshot(session).memory_directory); }
fs::path Directory(const std::shared_ptr<sdk::Session>& session) { return Memory(session).parent_path() / "sessions" / session->id(); }
sdk::Receipt Turn(const std::shared_ptr<sdk::Session>& session, const std::string& key,
    sdk::OperationState state = sdk::OperationState::Succeeded) {
    auto receipt = session->Submit(key, "SDKSaveNeedle"); REQUIRE(receipt.has_value());
    auto operation = session->WaitResult(receipt->operation_id, 30s); REQUIRE(operation.has_value());
    INFO(operation->error); CHECK(operation->state == state);
    CHECK(operation->result_persisted == (state != sdk::OperationState::Indeterminate)); return *receipt;
}
std::vector<sdk::memory::v1::SaveReport> Reports(const std::shared_ptr<sdk::Session>& session, const sdk::Receipt& receipt) {
    auto value = session->GetMemorySaves(receipt.operation_id); REQUIRE(value.has_value());
    for (const auto& r : *value) { CHECK(r.session_id == session->id()); CHECK(r.operation_id == receipt.operation_id); }
    return *value;
}
bool WaitRequested(const fs::path& directory, const std::string& session) {
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < until) {
        auto source = v3::ReadV3Ledger(directory / (session + ".jsonl"));
        if (source && std::any_of(source->events.begin(), source->events.end(), [](const auto& event) {
            return event.kind == v3::EventKindV3::MemorySaveRequested && event.payload.value("sdkMemoryWrite", Json()) == 1;
        })) return true;
        std::this_thread::sleep_for(10ms);
    } return false;
}
void Rewrite(const fs::path& path, const std::function<void(Json&)>& change) {
    auto j = Json::parse(Read(path)); j.erase("sha256"); change(j);
    j["sha256"] = lubancode::platform::Sha256Hex(j.dump()); Write(path, j.dump());
}
std::string RehashLedger(const std::string& original, const std::function<void(std::vector<Json>&)>& change) {
    std::istringstream input(original); std::vector<Json> lines; std::string text;
    while (std::getline(input, text)) { REQUIRE_FALSE(text.empty()); lines.push_back(Json::parse(text)); }
    REQUIRE_FALSE(lines.empty()); change(lines);
    std::uint64_t seq = 0; std::string previous(v3::kGenesisHash), output;
    for (auto& line : lines) {
        line["seq"] = ++seq; line.erase("prevHash"); line.erase("lineHash");
        const auto canonical = lubancode::trajectory::CanonicalJsonDump(line); REQUIRE(canonical.has_value());
        const auto hash = v3::ComputeLineHash(previous, *canonical);
        line["prevHash"] = previous; line["lineHash"] = hash;
        const auto full = lubancode::trajectory::CanonicalJsonDump(line); REQUIRE(full.has_value());
        output += *full + '\n'; previous = hash;
    }
    return output;
}
} // namespace

TEST_CASE("SDK memory_save: omitted and explicit off expose no save capability") {
    for (bool explicit_off : {false, true}) {
        Fixture f; auto trace = std::make_shared<Trace>(); auto runtime = sdk::Runtime::Create(f.Roots()); REQUIRE(runtime.has_value());
        auto options = Options(f, trace, false); if (explicit_off) options.memory_write = sdk::memory::v1::WriteOptions{false};
        auto session = (*runtime)->OpenSession(std::move(options)); REQUIRE(session.has_value()); CHECK_FALSE(Snapshot(*session).enabled);
        const auto receipt = Turn(*session, "off"); CHECK(Reports(*session, receipt).empty());
        CHECK(trace->models == 2); CHECK_FALSE(fs::exists(Memory(*session)));
        REQUIRE((*session)->Close().has_value()); CHECK(Reports(*session, receipt).empty());
        REQUIRE((*runtime)->Shutdown().has_value());
    }
}

TEST_CASE("SDK memory_save: actual Submit commits a formal topic after durable requested") {
    Fixture f; auto trace = std::make_shared<Trace>(); auto runtime = sdk::Runtime::Create(f.Roots()); REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(f, trace)); REQUIRE(session.has_value());
    CHECK_FALSE((*session)->DescribeMemory()->enabled); const auto receipt = Turn(*session, "save");
    auto reports = Reports(*session, receipt); REQUIRE(reports.size() == 1); const auto& r = reports.front();
    CHECK(r.state == "committed"); CHECK(r.attempt == 1); CHECK_FALSE(r.action_id.empty()); CHECK(r.action_id != "same-provider-id");
    CHECK(r.save_request_sha256.size() == 64); CHECK(r.request_sha256.size() == 64); CHECK(r.save_request_sha256 != r.request_sha256);
    REQUIRE_FALSE(r.stages.empty());
    CHECK((r.stages.back() == sdk::memory::v1::SaveStage{"result", "durable"})); CHECK(r.error.empty());
    CHECK(Read(Memory(*session) / lubancode::tools::Utf8ToPath(r.memory_path)).find("SDK_SAVE_OLD") != std::string::npos);
    const auto saved = Directory(*session).parent_path().parent_path() / "lifecycle" / r.commit_key;
    CHECK(lubancode::platform::Sha256Hex(Read(saved / "topic.snapshot.md")) == r.content_sha256);
    auto source = v3::ReadV3Ledger(Directory(*session) / ((*session)->id() + ".jsonl")); REQUIRE(source.has_value());
    const auto* requested = source->FindEvent(r.requested_event_id); const auto* receipted = source->FindEvent(r.receipted_event_id);
    REQUIRE(requested); REQUIRE(receipted); CHECK(requested->seq < receipted->seq); CHECK(requested->action_id == r.action_id);
    CHECK(requested->payload.at("saveRequestSha256").get<std::string>() == r.save_request_sha256);
    CHECK(receipted->payload.at("receipt").at("requestSha256").get<std::string>() == r.request_sha256);
    CHECK_FALSE(fs::exists(Memory(*session) / ".state" / "jobs")); CHECK(trace->models == 2);
    REQUIRE((*session)->Close().has_value()); CHECK(Reports(*session, receipt).front().request_sha256 == r.request_sha256);
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK memory_save: fact evidence and project feedback use shared formal validation") {
    for (const auto& kind : {"fact", "feedback"}) {
        Fixture f; auto trace = std::make_shared<Trace>(); auto input = Input(); input["kind"] = kind;
        input["id"] = std::string(kind) + ".sdk-save";
        if (std::string(kind) == "fact") { input["paths"] = Json::array({"evidence.txt"}); input["confidence"] = "verified"; }
        trace->inputs = {input}; auto runtime = sdk::Runtime::Create(f.Roots()); REQUIRE(runtime.has_value());
        auto session = (*runtime)->OpenSession(Options(f, trace)); REQUIRE(session.has_value());
        auto reports = Reports(*session, Turn(*session, kind)); REQUIRE(reports.size() == 1); CHECK(reports.front().state == "committed");
        CHECK(reports.front().memory_id == input.at("id").get<std::string>()); CHECK(trace->models == 2);
        REQUIRE((*runtime)->Shutdown().has_value());
    }
}

TEST_CASE("SDK memory_save: rejected payloads neither request nor mutate formal topics") {
    for (int variant = 0; variant != 5; ++variant) {
        Fixture f; auto trace = std::make_shared<Trace>(); auto input = Input();
        if (variant == 0) input["id"] = "../bad-id";
        if (variant == 1) input["content"] = std::string("bad\0text", 8);
        if (variant == 2) { input["kind"] = "feedback"; input["id"] = "feedback.sdk-save"; input["confidence"] = "inferred"; }
        if (variant == 3) input["scope"] = {{"kind", "path"}, {"value", "../outside"}};
        if (variant == 4) input["content"] = std::string(8193, 'x');
        trace->inputs = {input}; auto runtime = sdk::Runtime::Create(f.Roots()); REQUIRE(runtime.has_value());
        auto session = (*runtime)->OpenSession(Options(f, trace)); REQUIRE(session.has_value());
        const auto reports = Reports(*session, Turn(*session, "invalid")); REQUIRE(reports.size() == 1);
        CHECK(reports.front().state == "not_started"); CHECK(reports.front().requested_event_id.empty());
        CHECK(reports.front().stages.empty()); CHECK_FALSE(fs::exists(Memory(*session))); CHECK(trace->models == 2);
        REQUIRE((*runtime)->Shutdown().has_value());
    }
}

TEST_CASE("SDK memory_save: explicit restore freezes enabled and disabled plans") {
    for (bool enabled : {false, true}) {
        Fixture f; auto trace = std::make_shared<Trace>(); auto runtime = sdk::Runtime::Create(f.Roots()); REQUIRE(runtime.has_value());
        auto session = (*runtime)->OpenSession(Options(f, trace, enabled)); REQUIRE(session.has_value());
        const auto snap = Snapshot(*session); const auto receipt = Turn(*session, "before"); const auto reports = Reports(*session, receipt);
        REQUIRE((*session)->Close().has_value());
        auto options = Options(f, trace, false); options.resume_session_id = snap.session_id; options.system_prompt.clear();
        auto restored = (*runtime)->OpenSession(std::move(options)); REQUIRE(restored.has_value());
        CHECK(Snapshot(*restored).enabled == enabled); CHECK(Snapshot(*restored).plan_sha256 == snap.plan_sha256);
        CHECK(Reports(*restored, receipt).size() == reports.size()); CHECK(trace->models == 2);
        auto duplicate = (*restored)->Submit("before", "SDKSaveNeedle"); REQUIRE(duplicate.has_value()); CHECK(duplicate->duplicate); CHECK(trace->models == 2);
        REQUIRE((*restored)->Close().has_value());
        auto changed = Options(f, trace, false); changed.resume_session_id = snap.session_id; changed.memory_write = sdk::memory::v1::WriteOptions{!enabled};
        auto rejected = (*runtime)->OpenSession(std::move(changed)); REQUIRE_FALSE(rejected.has_value()); CHECK(rejected.error().code == "sdk.memory_write.resume_mismatch");
        REQUIRE((*runtime)->Shutdown().has_value());
    }
    // A real neutral/CLI-style V3 Session predates the SDK write plan. It may
    // continue off, but cannot gain a new plan through an explicit option.
    Fixture f; std::string legacy_id;
    {
        lubancode::runtime::TrajectorySessionLedger::Options launch;
        launch.workspace_root = f.root / "repo";
        launch.workspace_identity = lubancode::workspace::MakeFallbackIdentity(launch.workspace_root);
        launch.workspaces_root = f.root / "data" / "workspaces";
        launch.launch_cwd = Utf8(launch.workspace_root); launch.v3_system_content = "LEGACY_SAVE_OFF";
        auto ledger = lubancode::runtime::TrajectorySessionLedger::Open(launch); REQUIRE(ledger.has_value());
        legacy_id = ledger->session_id(); CHECK_FALSE(fs::exists(ledger->session_dir() / "sdk-memory-write-plan.json"));
        ledger->CloseSession("legacy fixture");
    }
    auto trace = std::make_shared<Trace>(); trace->inputs.clear();
    auto runtime = sdk::Runtime::Create(f.Roots()); REQUIRE(runtime.has_value());
    auto options = Options(f, trace, false); options.resume_session_id = legacy_id; options.system_prompt.clear();
    auto legacy = (*runtime)->OpenSession(std::move(options)); REQUIRE(legacy.has_value()); CHECK_FALSE(Snapshot(*legacy).enabled);
    CHECK(Reports(*legacy, Turn(*legacy, "legacy-off")).empty()); REQUIRE((*legacy)->Close().has_value());
    auto upgrade = Options(f, trace); upgrade.resume_session_id = legacy_id;
    auto refused = (*runtime)->OpenSession(std::move(upgrade)); REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == "sdk.memory_write.resume_mismatch"); CHECK(trace->models == 1);
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK memory_save: later topic updates preserve old immutable receipts") {
    Fixture f; auto trace = std::make_shared<Trace>(); trace->inputs = {Input(), Input("SDK_SAVE_NEW")};
    auto runtime = sdk::Runtime::Create(f.Roots()); REQUIRE(runtime.has_value()); auto session = (*runtime)->OpenSession(Options(f, trace)); REQUIRE(session.has_value());
    const auto old = Turn(*session, "old"); const auto before = Reports(*session, old).front();
    const auto next = Turn(*session, "new"); const auto after = Reports(*session, next).front();
    CHECK(before.action_id != after.action_id); CHECK(before.commit_key != after.commit_key); CHECK(old.operation_id != next.operation_id);
    CHECK(before.content_sha256 != after.content_sha256); CHECK(before.memory_path == after.memory_path);
    const auto life = Memory(*session).parent_path() / "lifecycle";
    CHECK(Read(life / before.commit_key / "topic.snapshot.md").find("SDK_SAVE_OLD") != std::string::npos);
    CHECK(Read(Memory(*session) / lubancode::tools::Utf8ToPath(after.memory_path)).find("SDK_SAVE_NEW") != std::string::npos);
    const auto id = (*session)->id(); REQUIRE((*session)->Close().has_value());
    auto options = Options(f, trace, false); options.resume_session_id = id; options.system_prompt.clear();
    auto restored = (*runtime)->OpenSession(std::move(options)); REQUIRE(restored.has_value());
    CHECK(Reports(*restored, old).front().content_sha256 == before.content_sha256); CHECK(trace->models == 4);
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK memory_save: rehashed report forgeries cannot claim or erase save facts") {
    Fixture f; auto trace = std::make_shared<Trace>(); trace->inputs = {Input(), Input("OTHER_TURN", "preference.other-turn")};
    auto runtime = sdk::Runtime::Create(f.Roots()); REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(f, trace)); REQUIRE(session.has_value());
    const auto receipt = Turn(*session, "saved"); const auto path = Directory(*session) / "sdk-memory-saves" / (receipt.operation_id + ".json");
    const auto saved = Reports(*session, receipt); REQUIRE(saved.size() == 1);
    const auto other = Reports(*session, Turn(*session, "other-turn")); REQUIRE(other.size() == 1);
    REQUIRE(saved.front().turn_id != other.front().turn_id);
    const auto journal = Directory(*session) / ((*session)->id() + ".jsonl");
    const auto original = Read(path), id = (*session)->id(); REQUIRE((*session)->Close().has_value());
    const auto original_journal = Read(journal); const auto model_calls = trace->models.load(); REQUIRE(model_calls == 4);
    const auto actual = v3::ReadV3Ledger(journal); REQUIRE(actual.has_value());
    const auto tool = std::find_if(actual->messages.begin(), actual->messages.end(), [&](const auto& message) {
        return message.action_id == saved.front().action_id && message.turn_id == saved.front().turn_id &&
            message.message.value("role", std::string()) == "tool" && message.result_selection_ref.has_value();
    });
    REQUIRE(tool != actual->messages.end());
    const auto tool_id = tool->message_id, selected_id = *tool->result_selection_ref;
    const auto* selected = actual->FindEvent(selected_id); REQUIRE(selected);
    REQUIRE(selected->kind == v3::EventKindV3::ToolResultSelected); REQUIRE(selected->seq < tool->seq);
    const auto started = std::find_if(actual->events.begin(), actual->events.end(), [&](const auto& event) {
        return event.kind == v3::EventKindV3::ToolExecutionStarted && event.action_id == saved.front().action_id;
    });
    REQUIRE(started != actual->events.end());
    for (int variant = 0; variant != 8; ++variant) {
        Write(path, original);
        Rewrite(path, [variant](Json& j) {
            if (variant == 0) j["reports"] = Json::array();
            else {
                auto& r = j["reports"][0];
                if (variant == 1) r["sessionId"] = "foreign";
                if (variant == 2) r["turnId"] = "foreign-turn";
                if (variant == 3) r["actionId"] = "same-provider-id";
                if (variant == 4) r["requestSha256"] = std::string(64, 'a');
                if (variant == 5) { r["state"] = "not_started"; r["stages"] = Json::array(); }
                if (variant == 6) r["stages"].back()["outcome"] = "unconfirmed";
                if (variant == 7) r["error"] = std::string("bad\0text", 8);
            }
        });
        auto options = Options(f, trace, false); options.resume_session_id = id;
        auto rejected = (*runtime)->OpenSession(std::move(options)); REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == "sdk.memory_write.open_failed"); CHECK(trace->models == model_calls);
        CHECK(Read(journal) == original_journal);
    }
    Write(path, original); fs::remove(path);
    auto missing = Options(f, trace, false); missing.resume_session_id = id;
    CHECK_FALSE((*runtime)->OpenSession(std::move(missing)).has_value());
    CHECK(trace->models == model_calls); CHECK(Read(journal) == original_journal); Write(path, original);
    // These are valid rehashed V3 files. The reader must accept them before the
    // SDK's adopted-result gate can prove that it rejects the forged linkage.
    for (int variant = 0; variant != 13; ++variant) {
        CAPTURE(variant); Write(journal, original_journal);
        const auto forged = RehashLedger(original_journal, [&](std::vector<Json>& lines) {
            auto selection = std::find_if(lines.begin(), lines.end(), [&](const auto& line) {
                return line.value("eventId", std::string()) == selected_id;
            });
            auto message = std::find_if(lines.begin(), lines.end(), [&](const auto& line) {
                return line.value("messageId", std::string()) == tool_id;
            });
            REQUIRE(selection != lines.end()); REQUIRE(message != lines.end());
            if (variant == 0) (*selection)["turnId"] = other.front().turn_id;
            if (variant == 1) std::iter_swap(selection, message);
            if (variant == 2) (*message)["message"]["role"] = "user";
            if (variant >= 3) {
                const auto slot = (variant - 3) % 5;
                const std::array<std::string, 5> ids{selected_id, tool_id, started->event_id,
                    saved.front().requested_event_id, saved.front().receipted_event_id};
                auto owner = std::find_if(lines.begin(), lines.end(), [&](const auto& line) {
                    return line.value(slot == 1 ? "messageId" : "eventId", std::string()) == ids[slot];
                });
                REQUIRE(owner != lines.end());
                (*owner)[variant < 8 ? "sessionId" : "runId"] = "foreign-owner";
            }
        });
        Write(journal, forged);
        const auto readable = v3::ReadV3Ledger(journal);
        INFO(readable ? std::string() : readable.error());
        REQUIRE(readable.has_value());
        const auto* forged_selection = readable->FindEvent(selected_id);
        const auto* forged_message = readable->FindMessage(tool_id);
        REQUIRE(forged_selection); REQUIRE(forged_message);
        if (variant == 0) CHECK(forged_selection->turn_id == other.front().turn_id);
        if (variant == 1) CHECK(forged_selection->seq > forged_message->seq);
        if (variant == 2) CHECK(forged_message->message.at("role").get<std::string>() == "user");
        if (variant >= 3) {
            const auto slot = (variant - 3) % 5;
            if (slot == 1) {
                CHECK((variant < 8 ? forged_message->session_id : forged_message->run_id) == "foreign-owner");
            } else {
                const std::array<std::string, 5> ids{selected_id, tool_id, started->event_id,
                    saved.front().requested_event_id, saved.front().receipted_event_id};
                const auto* forged_owner = readable->FindEvent(ids[slot]); REQUIRE(forged_owner);
                CHECK((variant < 8 ? forged_owner->session_id : forged_owner->run_id) == "foreign-owner");
            }
        }
        REQUIRE(std::any_of(readable->revision_chains.begin(), readable->revision_chains.end(), [&](const auto& revision) {
            return std::find(revision.second.second.begin(), revision.second.second.end(), tool_id) != revision.second.second.end();
        }));
        auto options = Options(f, trace, false); options.resume_session_id = id; options.system_prompt.clear();
        auto rejected = (*runtime)->OpenSession(std::move(options)); REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == "sdk.memory_write.open_failed"); CHECK(trace->models == model_calls);
        CHECK(Read(journal) == forged); CHECK(Read(path) == original);
        Write(journal, original_journal);
    }
    REQUIRE(v3::ReadV3Ledger(journal).has_value());
    auto correct = Options(f, trace, false); correct.resume_session_id = id; correct.system_prompt.clear();
    auto restored = (*runtime)->OpenSession(std::move(correct)); REQUIRE(restored.has_value());
    const auto recovered = Reports(*restored, receipt); REQUIRE(recovered.size() == 1);
    CHECK(recovered.front().receipted_event_id == saved.front().receipted_event_id);
    CHECK(trace->models == model_calls);
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK memory_save: corrupted immutable snapshot cannot be recovered as complete") {
    Fixture f; auto trace = std::make_shared<Trace>(); auto runtime = sdk::Runtime::Create(f.Roots()); REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(f, trace)); REQUIRE(session.has_value());
    const auto r = Reports(*session, Turn(*session, "saved")).front(); const auto id = (*session)->id();
    const auto snapshot = Memory(*session).parent_path() / "lifecycle" / r.commit_key / "topic.snapshot.md";
    const auto original = Read(snapshot); REQUIRE((*session)->Close().has_value()); Write(snapshot, "corrupt snapshot");
    auto options = Options(f, trace, false); options.resume_session_id = id;
    auto rejected = (*runtime)->OpenSession(std::move(options)); REQUIRE_FALSE(rejected.has_value()); CHECK(trace->models == 2);
    Write(snapshot, original); auto correct = Options(f, trace, false); correct.resume_session_id = id; correct.system_prompt.clear();
    auto restored = (*runtime)->OpenSession(std::move(correct)); REQUIRE(restored.has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK memory_save: unresolved gate prevents remaining batch and another model request") {
    Fixture f; auto trace = std::make_shared<Trace>(); trace->prime = true; trace->after = true;
    auto after = std::make_shared<std::atomic<unsigned>>(0); auto directory = std::make_shared<fs::path>(); auto session_id = std::make_shared<std::string>();
    auto runtime = sdk::Runtime::Create(f.Roots()); REQUIRE(runtime.has_value()); auto options = Options(f, trace);
    sdk::Tool prime; prime.name = "prime"; prime.requires_approval = false;
    prime.execute = [directory, session_id](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
        auto source = v3::ReadV3Ledger(*directory / (*session_id + ".jsonl"));
        if (!source) return std::unexpected(sdk::Error{"fixture.source", source.error()});
        std::string operation;
        for (const auto& f : lubancode::runtime::SessionService::ReadOperationFacts(*directory))
            if (f.kind == "operation.dispatched") operation = f.operation_id;
        for (const auto& e : source->events) if (e.kind == v3::EventKindV3::ToolExecutionPending && e.payload.value("toolName", Json()) == "memory_save") {
            const Json identity{{"session", *session_id}, {"operation", operation}, {"turn", *e.turn_id},
                {"action", *e.action_id}, {"attempt", e.payload.at("attempt")}};
            const auto key = "sdk-save-" + lubancode::platform::Sha256Hex(identity.dump());
            Write(directory->parent_path().parent_path() / "lifecycle" / key / "intent.json", "malformed prior intent");
            return sdk::ToolResult{"primed actual action", false};
        }
        return std::unexpected(sdk::Error{"fixture.action", "no actual pending save"});
    };
    sdk::Tool later; later.name = "after"; later.requires_approval = false;
    later.execute = [after](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> { ++*after; return sdk::ToolResult{"should not run", false}; };
    options.custom_tools.push_back(std::move(prime)); options.custom_tools.push_back(std::move(later));
    auto session = (*runtime)->OpenSession(std::move(options)); REQUIRE(session.has_value()); *directory = Directory(*session); *session_id = (*session)->id();
    const auto receipt = Turn(*session, "unknown", sdk::OperationState::Indeterminate);
    CHECK(trace->models == 1); CHECK(*after == 0); CHECK_FALSE(fs::exists(Memory(*session) / "preferences"));
    const auto reports = Reports(*session, receipt); REQUIRE(reports.size() == 1); CHECK(reports.front().state == "indeterminate");
    CHECK(reports.front().error_code == "memory.commit.invalid_record"); CHECK_FALSE((*session)->Submit("later", "must not execute").has_value());
    REQUIRE((*session)->Close().has_value()); CHECK(Reports(*session, receipt).front().state == "indeterminate");
    auto resumed_options = Options(f, trace, false); resumed_options.resume_session_id = *session_id; resumed_options.system_prompt.clear();
    auto resumed = (*runtime)->OpenSession(std::move(resumed_options)); REQUIRE(resumed.has_value());
    CHECK_FALSE((*resumed)->Submit("new-after-unknown", "must not execute").has_value()); CHECK(trace->models == 1);
    CHECK(Reports(*resumed, receipt).front().state == "indeterminate"); REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK memory_save: cancel and Close wait for the active project gate") {
    for (bool close : {false, true}) {
        Fixture f; auto trace = std::make_shared<Trace>(); auto runtime = sdk::Runtime::Create(f.Roots()); REQUIRE(runtime.has_value());
        auto session = (*runtime)->OpenSession(Options(f, trace)); REQUIRE(session.has_value());
        fs::create_directories(Memory(*session) / ".state"); lubancode::memory::OwnerLock held;
        auto acquired = lubancode::memory::OwnerLock::TryAcquire(Memory(*session) / ".state" / "memory.lock", &held);
        REQUIRE(acquired.status == lubancode::memory::OwnerLock::Status::Acquired);
        auto receipt = (*session)->Submit("held", "SDKSaveNeedle"); REQUIRE(receipt.has_value());
        REQUIRE(WaitRequested(Directory(*session), (*session)->id()));
        if (close) REQUIRE((*session)->Close().has_value()); else REQUIRE((*session)->Cancel(receipt->operation_id).has_value());
        auto operation = (*session)->WaitResult(receipt->operation_id, 10s); REQUIRE(operation.has_value()); INFO(operation->error);
        CHECK(operation->state == sdk::OperationState::Cancelled); CHECK(operation->result_persisted);
        auto reports = Reports(*session, *receipt); REQUIRE(reports.size() == 1); CHECK(reports.front().state == "not_started");
        CHECK(reports.front().error_code == "memory.commit.cancelled"); CHECK_FALSE(fs::exists(Memory(*session) / "preferences")); CHECK(trace->models == 1);
        REQUIRE((*session)->Close().has_value()); CHECK(Reports(*session, *receipt).size() == 1); REQUIRE((*runtime)->Shutdown().has_value());
    }
}

TEST_CASE("SDK memory_save: four Sessions retain local identities and project storage ownership") {
    Fixture f; std::array<std::shared_ptr<Trace>, 4> traces;
    for (std::size_t n = 0; n < traces.size(); ++n) { traces[n] = std::make_shared<Trace>(); traces[n]->inputs = {Input("SESSION_" + std::to_string(n), "preference.scene-" + std::to_string(n))}; }
    fs::create_directories(f.root / "other-a"); fs::create_directories(f.root / "other-b");
    auto runtime = sdk::Runtime::Create(f.Roots()); REQUIRE(runtime.has_value());
    std::array<std::shared_ptr<sdk::Session>, 4> sessions; std::array<sdk::Receipt, 4> receipts;
    std::array<fs::path, 4> cwd{f.root / "repo", f.root / "repo", f.root / "other-a", f.root / "other-b"};
    for (std::size_t n = 0; n < sessions.size(); ++n) { auto s = (*runtime)->OpenSession(Options(f, traces[n], true, cwd[n])); REQUIRE(s.has_value()); sessions[n] = *s; }
    for (std::size_t n = 0; n < sessions.size(); ++n) { auto r = sessions[n]->Submit("scene", "SDKSaveNeedle"); REQUIRE(r.has_value()); receipts[n] = *r; }
    for (std::size_t n = 0; n < sessions.size(); ++n) {
        auto operation = sessions[n]->WaitResult(receipts[n].operation_id, 30s); REQUIRE(operation.has_value()); INFO(operation->error);
        CHECK(operation->state == sdk::OperationState::Succeeded); CHECK(operation->result_persisted);
        const auto r = Reports(sessions[n], receipts[n]); REQUIRE(r.size() == 1);
        CHECK(r.front().workspace_key == Snapshot(sessions[n]).workspace_key); CHECK(r.front().memory_id == "preference.scene-" + std::to_string(n));
        CHECK(Read(Memory(sessions[n]) / lubancode::tools::Utf8ToPath(r.front().memory_path)).find("SESSION_" + std::to_string(n)) != std::string::npos);
        // Same local op string names the current Session's owned report.
        for (const auto& foreign : receipts) if (foreign.operation_id == receipts[n].operation_id)
            CHECK(sessions[n]->GetMemorySaves(foreign.operation_id)->front().session_id == sessions[n]->id());
    }
    CHECK(Memory(sessions[0]) == Memory(sessions[1])); CHECK(Memory(sessions[0]) != Memory(sessions[2])); CHECK(Memory(sessions[2]) != Memory(sessions[3]));
    for (auto& s : sessions) REQUIRE(s->Close().has_value()); REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK memory_save: strict request bridge refuses missing writer without a fallback reference") {
    Fixture f;
    lubancode::runtime::TrajectorySessionLedger::Options options;
    options.workspace_root = f.root / "repo";
    options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(options.workspace_root);
    options.workspaces_root = f.root / "data" / "workspaces";
    auto ledger = lubancode::runtime::TrajectorySessionLedger::Open(options); REQUIRE(ledger.has_value());
    auto actual_owner = std::move(*ledger); // the former owner now has no active writer
    REQUIRE(actual_owner.v3_main_writer() != nullptr);
    lubancode::runtime::MemoryLedgerBridge bridge(*ledger);
    const Json request = Input();
    const auto receipt = bridge.RecordSaveRequestedStrict({"session", "op-1", "turn-1", "action-1", 1}, request,
        lubancode::platform::Sha256Hex(request.dump()), "sdk-save-key");
    REQUIRE_FALSE(receipt.has_value()); CHECK(receipt.error() == "sdk.memory_write.invocation_invalid");
}
