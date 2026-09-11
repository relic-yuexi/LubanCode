// Failure-boundary audit probes, 2026-09-11.
// These characterize observed behavior, including known bugs; a green run is NOT
// an acceptance claim for durability. When fixing a finding, change the matching
// observation into the intended invariant. No real provider or user data is used.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_session.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"
#include "workspace/identity.hpp"
#include "turn_event_recorder.hpp"

using namespace lubancode;
namespace {
using Json = nlohmann::json;
namespace v3 = trajectory::v3;

// Default: pin observed behavior for review. Opt in to the intended invariants;
// those checks MUST fail until the corresponding production bugs are fixed.
bool ExpectFixed() {
    return platform::GetEnvVar("LUBANCODE_FAILURE_AUDIT_EXPECT_FIXED") == "1";
}

struct V3Environment {
    std::optional<std::string> previous =
        platform::GetEnvVar("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
    static void Set(const std::optional<std::string>& value) {
#ifdef _WIN32
        _putenv_s("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", value.value_or("").c_str());
#else
        if (value) setenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", value->c_str(), 1);
        else unsetenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
#endif
    }
    V3Environment() { Set(std::string("1")); }
    ~V3Environment() { Set(previous); }
};

struct TempLedger {
    std::filesystem::path path;
    bool keep = platform::GetEnvVar("LUBANCODE_FAILURE_AUDIT_KEEP") == "1";
    TempLedger() {
        static std::atomic<unsigned long long> serial{0};
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto candidate = std::filesystem::temp_directory_path() /
                ("lubancode-failure-audit-" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(serial++));
            // Claim a NEW directory atomically; never clean an existing path.
            if (std::filesystem::create_directory(candidate)) {
                path = std::filesystem::absolute(candidate).lexically_normal();
                return;
            }
        }
        throw std::runtime_error("cannot allocate failure audit temp directory");
    }
    ~TempLedger() {
        if (keep || path.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

class AuditBackend final : public api::Backend {
public:
    std::function<std::expected<void, api::Error>(int, const std::function<void(const api::StreamEvent&)>&)> emit;
    std::vector<api::Request> requests;
    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& sink,
        const std::atomic<bool>*) override {
        requests.push_back(request);
        return emit(static_cast<int>(requests.size()), sink);
    }
};

class AuditTool final : public tools::Tool {
public:
    int calls = 0;
    bool result_error = false;
    std::function<void()> effect;
    std::string name() const override { return "audit_tool"; }
    std::string description() const override { return "local audit counter"; }
    Json input_schema() const override { return Json::object(); }
    bool needs_confirm() const override { return false; }
    Result execute(const Json&) override {
        ++calls;
        if (effect) effect();
        return {result_error ? "AUDIT_ERROR" : "AUDIT_RESULT", result_error};
    }
};

void Reply(const std::function<void(const api::StreamEvent&)>& sink, bool tool = false,
           const std::string& call_id = "audit-call") {
    sink(api::MessageStart{"provider-response", "audit-model"});
    if (tool) {
        sink(api::ToolUseStart{0, call_id, "audit_tool"});
        sink(api::ToolUseInputDelta{0, "{}"});
    } else {
        sink(api::TextDelta{"SUCCESS"});
    }
    sink(api::ContentBlockDone{0});
    sink(api::MessageDone{tool ? "tool_use" : "end_turn", api::Usage{}});
}

api::Message Input() {
    api::Message input;
    input.role = api::Role::User;
    input.content.push_back(api::TextBlock{"audit task"});
    return input;
}

struct Audit {
    // Destroy bridge/writer before temp cleanup; restore the caller's v2/v3 flag.
    V3Environment environment;
    TempLedger temp;
    std::filesystem::path root = temp.path;
    std::optional<runtime::TrajectorySessionLedger> ledger;
    std::unique_ptr<runtime::TrajectoryTurnBridge> bridge;
    std::filesystem::path path;
    int writes = 0;
    int fail_at = 0;
    Audit() {
        runtime::TrajectorySessionLedger::Options options;
        options.workspaces_root = root / "workspaces";
        options.workspace_root = root / "ws";
        options.workspace_identity = workspace::MakeFallbackIdentity(root / "ws");
        options.launch_cwd = root.string();
        options.lubancode_version = "failure-audit";
        options.v3_system_content = "AUDIT_SYSTEM";
        auto opened = runtime::TrajectorySessionLedger::Open(options);
        REQUIRE(opened.has_value());
        ledger.emplace(std::move(*opened));
        REQUIRE(ledger->v3_main_writer() != nullptr);
        path = ledger->v3_main_writer()->path();
        bridge = ledger->NewTurnBridge({"audit", "openai-chat-completions", "terminal"});
        bridge->BeginTurn("audit-turn", "external_user");
        bridge->RecordInput(Input());
    }
    void Inject() {
        v3::V3WriterOptions options;
        options.inject_io_failure = [this]() -> std::optional<std::string> {
            ++writes;
            return fail_at > 0 && writes == fail_at ? std::optional<std::string>("audit") : std::nullopt;
        };
        auto replacement = v3::V3Writer::Continue(path, options);
        REQUIRE(replacement.has_value());
        *ledger->v3_main_writer() = std::move(*replacement);
    }
    agent::TurnWiring Wiring() {
        agent::TurnWiring wiring;
        wiring.boundary_recorder = bridge.get();
        wiring.wait_request_backoff = [](auto, auto) { return true; };
        wiring.on_tool_trace = [this](const auto& event) { bridge->OnToolTrace(event); };
        wiring.on_tool_results_committed = [this](const auto& batch, const auto& results) {
            bridge->OnToolResultsCommitted(batch, results);
        };
        return wiring;
    }
    std::vector<Json> Rows() const {
        std::ifstream in(path);
        std::vector<Json> rows;
        for (std::string line; std::getline(in, line);) {
            if (!line.empty()) rows.push_back(Json::parse(line));
        }
        return rows;
    }
    void Report(std::string id, Json facts) const {
        facts["id"] = id;
        facts["expect_fixed"] = ExpectFixed();
        facts["artifacts_retained"] = temp.keep;
        if (temp.keep) facts["ledger"] = path.string();
        facts["diagnostics"] = bridge->recent_errors();
        std::cout << "FAILURE_AUDIT " << facts.dump() << '\n';
    }
};

agent::AgentProfile Profile() {
    agent::AgentProfile profile;
    profile.request.model = "audit-model";
    profile.runtime.max_steps_per_turn = 3;
    profile.system_prompt = "AUDIT_SYSTEM";
    return profile;
}

int KindCount(const std::vector<Json>& rows, const std::string& kind) {
    int count = 0;
    for (const auto& row : rows) if (row.value("kind", "") == kind) ++count;
    return count;
}
}  // namespace

TEST_CASE("failure audit FA-03: prepared and sent write gates") {
    for (const int fail_at : {1, 2}) {
        CAPTURE(fail_at);
        Audit audit;
        audit.Inject();
        audit.fail_at = fail_at;
        AuditBackend backend;
        backend.emit = [](int, const auto& sink) -> std::expected<void, api::Error> {
            Reply(sink);
            return {};
        };
        tools::ToolRegistry registry;
        agent::Agent agent(backend, registry, Profile());
        const auto result = agent.Run(Input(), audit.Wiring());
        CHECK_FALSE(result.has_value());
        CHECK(audit.ledger->v3_main_writer()->broken());
        CHECK(backend.requests.size() == (fail_at == 1 || ExpectFixed() ? 0 : 1));
        REQUIRE_FALSE(audit.bridge->recent_errors().empty());
        CHECK(audit.bridge->recent_errors().front().find(
            fail_at == 1 ? "model.request.prepared:" : "model.request.sent:") == 0);
        audit.Report(fail_at == 1 ? "control_prepared" : "sent_gate", {
            {"backend_calls", backend.requests.size()}, {"run_ok", result.has_value()},
            {"writer_broken", audit.ledger->v3_main_writer()->broken()}});
    }
}

TEST_CASE("failure audit control: assistant write failure blocks tool execution") {
    Audit audit;
    audit.Inject();
    AuditBackend backend;
    backend.emit = [&audit](int, const auto& sink) -> std::expected<void, api::Error> {
        Reply(sink, true);
        audit.fail_at = audit.writes + 1;
        return {};
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<AuditTool>();
    auto* counter = tool.get();
    registry.Register(std::move(tool));
    agent::Agent agent(backend, registry, Profile());
    const auto result = agent.Run(Input(), audit.Wiring());
    CHECK_FALSE(result.has_value());
    CHECK(counter->calls == 0);
    CHECK(backend.requests.size() == 1);
    REQUIRE_FALSE(audit.bridge->recent_errors().empty());
    CHECK(audit.bridge->recent_errors().front().find("model.response.completed:") == 0);
    audit.Report("control_output", {{"run_ok", result.has_value()}, {"tool_calls", counter->calls}});
}

TEST_CASE("failure audit FA-04 FA-05: retry terminals and visible text") {
    Audit audit;
    AuditBackend backend;
    backend.emit = [](int attempt, const auto& sink) -> std::expected<void, api::Error> {
        if (attempt == 1) {
            sink(api::MessageStart{"failed-response", "audit-model"});
            sink(api::TextDelta{std::string(5000, 'X')});
            return std::unexpected(api::Error{api::ErrorKind::Network, "audit reset", 0});
        }
        Reply(sink);
        return {};
    };
    tools::ToolRegistry registry;
    agent::Agent agent(backend, registry, Profile());
    test::RecordedTurn displayed;
    auto wiring = audit.Wiring();
    wiring.events = &displayed.adapter;
    const auto result = agent.Run(Input(), wiring);
    REQUIRE(result.has_value());
    const auto rows = audit.Rows();
    CHECK(backend.requests.size() == 2);
    CHECK(KindCount(rows, "model.response.started") == 2);
    CHECK(KindCount(rows, "model.response.completed") == 1);
    CHECK(KindCount(rows, "model.response.failed") == (ExpectFixed() ? 1 : 0));
    // A terminal must refer to its own physical request, not merely have a count.
    if (ExpectFixed()) {
        for (const auto& started : rows) {
            if (started.value("kind", "") != "model.response.started") continue;
            const auto request_id = started.at("requestId");
            int terminals = 0;
            for (const auto& row : rows) {
                const auto kind = row.value("kind", "");
                if ((kind == "model.response.failed" || kind == "model.response.completed" ||
                     kind == "model.response.cancelled") && row.at("requestId") == request_id)
                    ++terminals;
            }
            CHECK(terminals == 1);
        }
    }
    std::string history_text;
    for (const auto& message : agent.history()) if (message.role == api::Role::Assistant)
        for (const auto& block : message.content)
            if (const auto* text = std::get_if<api::TextBlock>(&block)) history_text += text->text;
    std::set<std::string> displayed_text_items;
    for (const auto& event : displayed.recorder.events)
        if (event.kind == runtime::ServerEventKind::ItemDelta && event.item_kind == runtime::ItemKind::Text)
            displayed_text_items.insert(event.item_id);
    CHECK(history_text == "SUCCESS");
    // Observer scope: TurnEventAdapter text projection, not a native-screen test.
    CHECK(displayed.recorder.text == (ExpectFixed() ? "SUCCESS" : std::string(5000, 'X') + "SUCCESS"));
    if (!ExpectFixed()) CHECK(displayed_text_items.size() == 1);
    audit.Report("retry_terminal", {{"backend_calls", backend.requests.size()},
        {"started", KindCount(rows, "model.response.started")},
        {"completed", KindCount(rows, "model.response.completed")},
        {"failed", KindCount(rows, "model.response.failed")},
        {"display_text_bytes", displayed.recorder.text.size()},
        {"display_contains_failed_prefix", displayed.recorder.text.starts_with(std::string(5000, 'X'))},
        {"display_text_items", displayed_text_items.size()}, {"history_text", history_text},
        {"verify_ok", v3::VerifyV3File(audit.path).ok}});
}

TEST_CASE("failure audit control: metadata failure retains journal result") {
    Audit audit;
    AuditBackend backend;
    backend.emit = [](int attempt, const auto& sink) -> std::expected<void, api::Error> {
        Reply(sink, attempt <= 2, "audit-call-" + std::to_string(attempt));
        return {};
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<AuditTool>();
    auto* counter = tool.get();
    tool->effect = [&audit, counter] {
        if (counter->calls != 2) return;
        // Obstruct one metadata destination in this fresh test ledger, not a real disk.
        const auto target = audit.path.parent_path() / "artifacts" / "res-000002.json";
        std::filesystem::create_directories(target);
        std::ofstream(target / "keep") << "audit";
    };
    registry.Register(std::move(tool));
    agent::Agent agent(backend, registry, Profile());
    const auto result = agent.Run(Input(), audit.Wiring());
    const auto rows = audit.Rows();
    const auto replay = audit.ledger->ProjectV3ContextHistory();
    const auto resumed = v3::ProjectResume(audit.path);
    int replay_results = 0;
    if (replay) for (const auto& message : *replay) for (const auto& block : message.content)
        if (std::holds_alternative<api::ToolResultBlock>(block)) ++replay_results;
    int sent_results = 0;
    if (backend.requests.size() > 1) for (const auto& message : backend.requests.back().messages)
        for (const auto& block : message.content)
            if (std::holds_alternative<api::ToolResultBlock>(block)) ++sent_results;
    audit.Report("result_store", {{"run_ok", result.has_value()}, {"tool_calls", counter->calls},
        {"backend_calls", backend.requests.size()}, {"sent_results", sent_results},
        {"persist_failed", KindCount(rows, "tool.result.persist_failed")},
        {"selected", KindCount(rows, "tool.result.selected")}, {"replay_ok", replay.has_value()},
        {"replay_results", replay_results}, {"verify_ok", v3::VerifyV3File(audit.path).ok},
        {"resume_ok", resumed.has_value()},
        {"open_actions", resumed ? static_cast<int>(resumed->execution.open_actions.size()) : -1}});
    CHECK(counter->calls == 2);
    CHECK(KindCount(rows, "tool.result.persist_failed") == 1);
    REQUIRE(resumed.has_value());
    // Control: metadata persistence failed, but the main journal retained the
    // result. Whether this fallback may continue is a policy question, not the
    // missing-live-input bug in the unavailable-store test below.
    CHECK(result.has_value());
    CHECK(backend.requests.size() == 3);
    CHECK(sent_results == 2);
    CHECK(KindCount(rows, "tool.result.selected") == 1);
    REQUIRE(replay.has_value());
    CHECK(replay_results == 2);
    CHECK(resumed->execution.open_actions.empty());
}

TEST_CASE("failure audit FA-01: unavailable store and recovery work") {
    Audit audit;
    AuditBackend backend;
    backend.emit = [](int attempt, const auto& sink) -> std::expected<void, api::Error> {
        Reply(sink, attempt == 1);
        return {};
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<AuditTool>();
    auto* counter = tool.get();
    tool->effect = [&audit] {
        const auto artifacts = audit.path.parent_path() / "artifacts";
        if (std::filesystem::is_directory(artifacts)) {
            REQUIRE(std::filesystem::is_empty(artifacts));
            std::filesystem::remove(artifacts);  // empty directory inside this new test fixture only
        }
        std::ofstream(artifacts) << "blocks ResultStore::Open";
    };
    registry.Register(std::move(tool));
    agent::Agent agent(backend, registry, Profile());
    const auto result = agent.Run(Input(), audit.Wiring());
    const auto rows = audit.Rows();
    const auto replay = audit.ledger->ProjectV3ContextHistory();
    const auto resumed = v3::ProjectResume(audit.path);
    int disk_tool_messages = 0;
    for (const auto& row : rows)
        if (row.contains("message") && row.at("message").value("role", "") == "tool")
            ++disk_tool_messages;
    int sent_results = 0;
    if (backend.requests.size() > 1) for (const auto& message : backend.requests.back().messages)
        for (const auto& block : message.content)
            if (std::holds_alternative<api::ToolResultBlock>(block)) ++sent_results;
    audit.Report("result_store_unavailable", {{"run_ok", result.has_value()},
        {"backend_calls", backend.requests.size()}, {"tool_calls", counter->calls},
        {"sent_results", sent_results}, {"disk_tool_messages", disk_tool_messages},
        {"replay_ok", replay.has_value()}, {"verify_ok", v3::VerifyV3File(audit.path).ok},
        {"resume_ok", resumed.has_value()},
        {"open_actions", resumed ? static_cast<int>(resumed->execution.open_actions.size()) : -1}});
    CHECK(counter->calls == 1);
    CHECK(disk_tool_messages == 0);
    REQUIRE(resumed.has_value());
    REQUIRE(replay.has_value());
    if (ExpectFixed()) {
        CHECK_FALSE(result.has_value());
        CHECK(backend.requests.size() == 1);
        CHECK_FALSE(resumed->execution.open_actions.empty());
    } else {
        CHECK(result.has_value());
        CHECK(backend.requests.size() == 2);
        CHECK(sent_results == 1);
        CHECK(resumed->execution.open_actions.empty());
    }
    int replay_results = 0;
    for (const auto& message : *replay) for (const auto& block : message.content)
        if (std::holds_alternative<api::ToolResultBlock>(block)) ++replay_results;
    CHECK(replay_results == 0);
}

TEST_CASE("failure audit FA-02: failed tool result error flag roundtrip") {
    Audit audit;
    AuditBackend backend;
    backend.emit = [](int attempt, const auto& sink) -> std::expected<void, api::Error> {
        Reply(sink, attempt == 1);
        return {};
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<AuditTool>();
    tool->result_error = true;
    registry.Register(std::move(tool));
    agent::Agent agent(backend, registry, Profile());
    const auto result = agent.Run(Input(), audit.Wiring());
    REQUIRE(result.has_value());
    REQUIRE(backend.requests.size() == 2);
    const auto replay = audit.ledger->ProjectV3ContextHistory();
    REQUIRE(replay.has_value());
    auto errors = [](const std::vector<api::Message>& messages) {
        int count = 0;
        for (const auto& message : messages) for (const auto& block : message.content)
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block); result && result->is_error)
                ++count;
        return count;
    };
    const int live_errors = errors(backend.requests.back().messages);
    const int replay_errors = errors(*replay);
    audit.Report("tool_error_flag", {{"live_error_results", live_errors},
        {"replay_error_results", replay_errors}, {"verify_ok", v3::VerifyV3File(audit.path).ok}});
    CHECK(live_errors == 1);
    CHECK(replay_errors == (ExpectFixed() ? live_errors : 0));
}
