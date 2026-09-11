// Failure-boundary audit probes, 2026-09-11.
// These characterize observed behavior, including known bugs; a green run is NOT
// an acceptance claim for durability. When fixing a finding, change the matching
// observation into the intended invariant. No real provider or user data is used.
//
// 2026-09-11 P1 batch (失败与恢复单): FA-01/FA-02/FA-03 observation branches
// were replaced by the intended invariants (unconditional CHECKs) — the fixes
// landed in loop.cpp / trajectory_session.cpp / reader.cpp. FA-04/FA-05 remain
// probes (P2, still gated by LUBANCODE_FAILURE_AUDIT_EXPECT_FIXED=1). The
// original observation snapshot stays in interview/failure-and-recovery.md.
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
#include "api/chat/request.hpp"
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
    std::string SerializeForDiagnostics(const api::Request& request) const override {
        return api::chat::BuildRequestJson(request).dump();
    }
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
    std::optional<std::string> result_content;
    std::string result_outcome;
    std::vector<tools::ToolContentBlock> native_blocks;
    std::function<void()> effect;
    std::string name() const override { return "audit_tool"; }
    std::string description() const override { return "local audit counter"; }
    Json input_schema() const override { return Json::object(); }
    bool needs_confirm() const override { return false; }
    Result execute(const Json&) override {
        ++calls;
        if (effect) effect();
        Result result{result_content.value_or(result_error ? "AUDIT_ERROR" : "AUDIT_RESULT"), result_error};
        result.outcome = result_outcome;
        if (!native_blocks.empty()) {
            auto payload = result.payload;
            payload.content = native_blocks;
            result.SetPayload(std::move(payload));
        }
        return result;
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
        wiring.capture_tool_result = [this](const api::ToolResultBlock& result) {
            return bridge->CaptureToolResult(result);
        };
        // P1-A:回执口(与 ToolTraceHub::Install 同款)——提交成败交回引擎。
        wiring.on_tool_results_committed_receipt =
            [this](const std::string& batch, const api::Message& results) {
                return bridge->OnToolResultsCommitted(batch, results);
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
    // Fixed 2026-09-11 (P1-C): the pre-send write gate is wired through
    // LoopBoundaryRecorder::OnRequestSent returning false — a failed sent write
    // must keep the backend at zero calls (prepared failure already did).
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
        CHECK(backend.requests.size() == 0);  // 本地账写不动,一次都不发
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

TEST_CASE("failure audit: metadata failure stops before unpublished result is sent") {
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
    // B1 requires immutable source metadata before publication; the first
    // committed result survives and the second result never reaches the model.
    CHECK_FALSE(result.has_value());
    CHECK(backend.requests.size() == 2);
    CHECK(sent_results == 1);
    CHECK(KindCount(rows, "tool.result.selected") == 1);
    REQUIRE(replay.has_value());
    CHECK(replay_results == 1);
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
    // Fixed 2026-09-11 (P1-A): the batch receipt gate stops the loop before a
    // second model request, and the resume projection lists the result gap
    // (execution terminal kept, result chain missing) as recovery work.
    CHECK_FALSE(result.has_value());
    CHECK(backend.requests.size() == 1);
    REQUIRE(resumed->execution.open_actions.size() == 1);
    CHECK(resumed->execution.open_actions[0].folded_status == "result_missing");
    REQUIRE_FALSE(resumed->execution.open_actions[0].attempts.empty());
    CHECK(resumed->execution.open_actions[0].attempts.back().status == "done");
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
    // Fixed 2026-09-11 (P1-B): the fed-back error semantics ride the final tool
    // message body and come back through the projection unchanged.
    CHECK(replay_errors == live_errors);
}

TEST_CASE("B1 real loop: 2 MiB captures persist and fixed previews match ledger wire and resume") {
    Audit audit;
    AuditBackend backend;
    backend.emit = [](int attempt, const auto& sink) -> std::expected<void, api::Error> {
        Reply(sink, attempt <= 2, "audit-call-" + std::to_string(attempt));
        return {};
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<AuditTool>();
    auto* counter = tool.get();
    const std::string original = "HEAD-中文\n" + std::string(2 * 1024 * 1024, 'x') + "\nTAIL-原文";
    tool->result_content = original;
    bool complete = true;
    SUBCASE("complete capture") {}
    SUBCASE("capture quota") {
        complete = false;
        tool->result_outcome = "output_limit";
        tool->result_error = true;
    }
    registry.Register(std::move(tool));
    agent::Agent agent(backend, registry, Profile());
    auto wiring = audit.Wiring();
    wiring.rewrite_tool_results_for_history = [&audit](api::Message& results) {
        return audit.bridge->RewriteToolResultsForHistory(results);
    };
    const auto outcome = agent.Run(Input(), wiring);
    REQUIRE(outcome.has_value());
    CHECK(counter->calls == 2);
    REQUIRE(backend.requests.size() == 3);
    std::vector<std::string> sent;
    for (std::size_t request_index = 1; request_index < backend.requests.size(); ++request_index) {
        const auto wire = api::chat::BuildRequestJson(backend.requests[request_index]);
        std::vector<std::string> this_request;
        for (const auto& message : wire.at("messages")) {
            if (message.value("role", "") != "tool") continue;
            this_request.push_back(message.at("content").get<std::string>());
            CHECK(this_request.back().size() <= 32768);
            CHECK(this_request.back().find(complete ? "capture_complete: true" : "capture_complete: false") != std::string::npos);
        }
        REQUIRE(this_request.size() == request_index);
        if (!sent.empty()) CHECK(this_request.front() == sent.front());
        sent = std::move(this_request);
    }
    std::vector<std::string> durable;
    for (const auto& row : audit.Rows()) {
        if (row.value("type", "") == "message" && row.at("message").value("role", "") == "tool")
            durable.push_back(row.at("message").at("content").get<std::string>());
    }
    CHECK(durable == sent);
    const auto resumed = audit.ledger->ProjectV3ContextHistory();
    REQUIRE(resumed.has_value());
    std::vector<std::string> replayed;
    for (const auto& message : *resumed) for (const auto& block : message.content) {
        if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) replayed.push_back(result->content);
    }
    CHECK(replayed == sent);
    for (const auto* name : {"res-000001.combined.txt", "res-000002.combined.txt"}) {
        std::ifstream stream(audit.path.parent_path() / "artifacts" / name, std::ios::binary);
        const std::string saved(std::istreambuf_iterator<char>(stream), {});
        CHECK(saved == original);
    }
    CHECK(v3::VerifyV3File(audit.path).ok);
}

TEST_CASE("B1 real loop: failed immutable metadata never publishes the next tool result") {
    Audit audit;
    AuditBackend backend;
    backend.emit = [](int attempt, const auto& sink) -> std::expected<void, api::Error> {
        Reply(sink, attempt <= 2, "audit-call-" + std::to_string(attempt));
        return {};
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<AuditTool>();
    auto* counter = tool.get();
    tool->result_content = std::string(2 * 1024 * 1024, 'x');
    tool->effect = [&audit, counter] {
        if (counter->calls == 2) {
            const auto destination = audit.path.parent_path() / "artifacts" / "res-000002.json";
            std::filesystem::create_directories(destination);
            std::ofstream(destination / "keep") << "blocked";
        }
    };
    registry.Register(std::move(tool));
    agent::Agent agent(backend, registry, Profile());
    auto wiring = audit.Wiring();
    wiring.rewrite_tool_results_for_history = [&audit](api::Message& results) {
        return audit.bridge->RewriteToolResultsForHistory(results);
    };
    CHECK_FALSE(agent.Run(Input(), wiring).has_value());
    CHECK(counter->calls == 2);
    CHECK(backend.requests.size() == 2);
    CHECK(KindCount(audit.Rows(), "tool.result.persist_failed") == 1);
    int published = 0;
    for (const auto& message : agent.history()) for (const auto& block : message.content)
        if (std::holds_alternative<api::ToolResultBlock>(block)) ++published;
    CHECK(published == 1);
    CHECK(std::filesystem::file_size(audit.path.parent_path() / "artifacts" / "res-000002.combined.txt") == 2 * 1024 * 1024);
}

TEST_CASE("B1 real loop: every preview ledger write failure stops before publication") {
    for (const int fail_at : {1, 2, 3, 4}) {
        CAPTURE(fail_at);
        Audit audit;
        AuditBackend backend;
        backend.emit = [](int attempt, const auto& sink) -> std::expected<void, api::Error> {
            Reply(sink, attempt == 1);
            return {};
        };
        tools::ToolRegistry registry;
        auto tool = std::make_unique<AuditTool>();
        auto* counter = tool.get();
        tool->result_content = std::string(2 * 1024 * 1024, 'x');
        registry.Register(std::move(tool));
        agent::Agent agent(backend, registry, Profile());
        auto wiring = audit.Wiring();
        wiring.rewrite_tool_results_for_history = [&audit, fail_at](api::Message& results) {
            audit.Inject();
            audit.fail_at = fail_at;
            return audit.bridge->RewriteToolResultsForHistory(results);
        };
        const auto outcome = agent.Run(Input(), wiring);
        CHECK_FALSE(outcome.has_value());
        CHECK(counter->calls == 1);
        CHECK(backend.requests.size() == 1);
        CHECK(audit.writes >= fail_at);
        CHECK(audit.ledger->v3_main_writer()->broken());
        int results = 0;
        for (const auto& message : agent.history()) for (const auto& block : message.content)
            if (std::holds_alternative<api::ToolResultBlock>(block)) ++results;
        CHECK(results == 0);
        CHECK(std::filesystem::file_size(audit.path.parent_path() / "artifacts" / "res-000001.combined.txt") == 2 * 1024 * 1024);
    }
}

TEST_CASE("B1 real loop: terminal event failure cannot bypass preview commit") {
    Audit audit;
    AuditBackend backend;
    backend.emit = [](int attempt, const auto& sink) -> std::expected<void, api::Error> {
        Reply(sink, attempt == 1);
        return {};
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<AuditTool>();
    auto* counter = tool.get();
    registry.Register(std::move(tool));
    agent::Agent agent(backend, registry, Profile());
    auto wiring = audit.Wiring();
    wiring.on_tool_trace = [&audit](const agent::ToolTraceEvent& event) {
        if (event.kind == agent::ToolTraceEventKind::ExecutionFinished) {
            audit.Inject();
            audit.fail_at = 1;
        }
        audit.bridge->OnToolTrace(event);
    };
    wiring.rewrite_tool_results_for_history = [&audit](api::Message& results) {
        return audit.bridge->RewriteToolResultsForHistory(results);
    };
    CHECK_FALSE(agent.Run(Input(), wiring).has_value());
    CHECK(counter->calls == 1);
    CHECK(backend.requests.size() == 1);
}

TEST_CASE("B1 publication failure after commit recovers the committed preview without rerunning tools") {
    Audit audit;
    AuditBackend backend;
    backend.emit = [](int attempt, const auto& sink) -> std::expected<void, api::Error> {
        Reply(sink, attempt == 1);
        return {};
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<AuditTool>();
    auto* counter = tool.get();
    tool->result_content = std::string(2 * 1024 * 1024, 'x');
    registry.Register(std::move(tool));
    agent::Agent agent(backend, registry, Profile());
    auto wiring = audit.Wiring();
    wiring.rewrite_tool_results_for_history = [&audit](api::Message& results) {
        auto receipt = audit.bridge->RewriteToolResultsForHistory(results);
        REQUIRE(receipt.status == runtime::ToolResultsCommitReceipt::Status::Committed);
        // Inject the stop after durable admission and before loop PushMessage.
        receipt.status = runtime::ToolResultsCommitReceipt::Status::Failed;
        receipt.error_code = "injected.publication_failure";
        return receipt;
    };
    CHECK_FALSE(agent.Run(Input(), wiring).has_value());
    CHECK(backend.requests.size() == 1);
    CHECK(counter->calls == 1);
    auto recovered = audit.ledger->ProjectV3ContextHistory();
    REQUIRE(recovered.has_value());
    std::string committed_preview;
    for (const auto& message : *recovered) for (const auto& block : message.content)
        if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) committed_preview = result->content;
    REQUIRE_FALSE(committed_preview.empty());
    CHECK(committed_preview.size() <= 32768);
    AuditBackend resumed_backend;
    resumed_backend.emit = [](int, const auto& sink) -> std::expected<void, api::Error> {
        Reply(sink);
        return {};
    };
    agent::Agent resumed_agent(resumed_backend, registry, Profile());
    resumed_agent.RestoreSessionHistory(std::move(*recovered));
    REQUIRE(resumed_agent.Run("continue", agent::TurnWiring{}).has_value());
    REQUIRE(resumed_backend.requests.size() == 1);
    const auto wire = api::chat::BuildRequestJson(resumed_backend.requests[0]);
    int tool_results = 0;
    for (const auto& message : wire.at("messages")) if (message.value("role", "") == "tool") {
        ++tool_results;
        CHECK(message.at("content") == committed_preview);
    }
    CHECK(tool_results == 1);
    CHECK(counter->calls == 1);
}

TEST_CASE("B2 real loop: action summaries use separate requests and adopted results survive resume") {
    Audit audit;
    AuditBackend backend;
    backend.emit = [&audit](int call, const auto& sink) -> std::expected<void, api::Error> {
        if (call == 1) {
            sink(api::MessageStart{"provider-response", "audit-model"});
            for (int index = 0; index < 2; ++index) {
                sink(api::ToolUseStart{index, "summary-tool-" + std::to_string(index), "audit_tool"});
                sink(api::ToolUseInputDelta{index, "{}"});
                sink(api::ContentBlockDone{index});
            }
            sink(api::MessageDone{"tool_use", api::Usage{}});
        } else if (call <= 3) {
            if (call == 2) {
                int raw_captures = 0;
                for (const auto& row : audit.Rows()) {
                    if (row.value("kind", "") != "tool.result.persisted") continue;
                    bool raw = false;
                    for (const auto& ref : row.at("payload").at("result_ref")) {
                        if (ref.at("path").get<std::string>().find("capture-") != std::string::npos) raw = true;
                    }
                    if (raw) ++raw_captures;
                }
                CHECK(raw_captures == 2);
            }
            sink(api::MessageStart{"summary-response", "audit-model"});
            sink(api::TextDelta{R"({"summary":"inspection completed","side_effects":["read only"],"open_items":["review evidence"],"evidence":["combined output"]})"});
            sink(api::ContentBlockDone{0});
            sink(api::MessageDone{"end_turn", api::Usage{123, 45, 0, 0, 0}, true});
        } else {
            Reply(sink);
        }
        return {};
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<AuditTool>();
    auto* counter = tool.get();
    tool->result_content = std::string(16000, 'x');
    registry.Register(std::move(tool));
    auto profile = Profile();
    profile.provider = "audit";
    profile.prompt_sections.wire = "openai-chat-completions";
    profile.runtime.context_window_tokens = 9000;
    profile.runtime.max_output_tokens = 1024;
    profile.runtime.max_output_tokens_source = agent::OutputBudgetSource::ConfigFile;
    agent::Agent agent(backend, registry, profile);
    auto wiring = audit.Wiring();
    wiring.configure_action_summary = [&audit](api::Backend* selected, const runtime::ActionSummaryProfile& summary_profile) {
        audit.bridge->ConfigureActionSummary(selected, summary_profile);
    };
    wiring.rewrite_tool_results_for_history = [&audit](api::Message& results) {
        return audit.bridge->RewriteToolResultsForHistory(results);
    };
    const auto outcome = agent.Run(Input(), wiring);
    REQUIRE_MESSAGE(outcome.has_value(), outcome.error());
    CHECK(counter->calls == 2);
    REQUIRE(backend.requests.size() == 4);
    CHECK(KindCount(audit.Rows(), "tool.result.summary.finished") == 2);
    auto recovered = audit.ledger->ProjectV3ContextHistory();
    REQUIRE(recovered.has_value());
    api::Request replay = backend.requests.back();
    replay.messages = *recovered;
    const auto wire = api::chat::BuildRequestJson(backend.requests.back());
    const auto resumed = api::chat::BuildRequestJson(replay);
    std::vector<Json> sent_tools, resumed_tools;
    for (const auto& message : wire.at("messages")) if (message.value("role", "") == "tool") sent_tools.push_back(message);
    for (const auto& message : resumed.at("messages")) if (message.value("role", "") == "tool") resumed_tools.push_back(message);
    CHECK(sent_tools == resumed_tools);
    REQUIRE(sent_tools.size() == 2);
    for (const auto& tool_message : sent_tools) {
        const auto body = Json::parse(tool_message.at("content").get<std::string>());
        CHECK(body.at("execution_already_occurred") == true);
        CHECK(body.at("execution_state") == "done");
        CHECK(body.at("capture_complete") == true);
        CHECK(body.at("evidence_paths").size() >= 2);
        CHECK(body.at("source_result_event_refs").size() == 2);
    }
    auto ledger = v3::ReadV3Ledger(audit.path);
    REQUIRE(ledger.has_value());
    int summary_responses = 0;
    for (const auto& message : ledger->messages) {
        if (message.purpose == v3::MessagePurpose::ActionSummary && message.message.at("role") == "assistant") {
            ++summary_responses;
            CHECK(message.usage->at("inputTokens") == 123);
        }
    }
    CHECK(summary_responses == 2);
    for (const char* name : {"res-000001.combined.txt", "res-000002.combined.txt"}) {
        CHECK(std::filesystem::file_size(audit.path.parent_path() / "artifacts" / name) == 16000);
    }
}

TEST_CASE("B1 real loop: multi-file native text shares one preview and retains both originals") {
    Audit audit;
    AuditBackend backend;
    backend.emit = [](int attempt, const auto& sink) -> std::expected<void, api::Error> {
        Reply(sink, attempt == 1);
        return {};
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<AuditTool>();
    auto* counter = tool.get();
    tool->result_content = "two resource files";
    tools::EmbeddedTextResourceContent first;
    first.uri = "file:///first.txt";
    first.mime_type = "text/plain";
    first.text = "第一份\n" + std::string(1024 * 1024, 'a');
    tools::EmbeddedTextResourceContent second = first;
    second.uri = "file:///second.txt";
    second.text = "第二份\n" + std::string(1024 * 1024, 'b');
    tool->native_blocks = {first, second};
    registry.Register(std::move(tool));
    agent::Agent agent(backend, registry, Profile());
    auto wiring = audit.Wiring();
    wiring.rewrite_tool_results_for_history = [&audit](api::Message& results) {
        return audit.bridge->RewriteToolResultsForHistory(results);
    };
    REQUIRE(agent.Run(Input(), wiring).has_value());
    REQUIRE(backend.requests.size() == 2);
    CHECK(counter->calls == 1);
    std::string preview;
    const auto wire = api::chat::BuildRequestJson(backend.requests[1]);
    for (const auto& message : wire.at("messages")) if (message.value("role", "") == "tool")
        preview = message.at("content").get<std::string>();
    REQUIRE_FALSE(preview.empty());
    CHECK(preview.size() <= 32768);
    CHECK(preview.find("res-000001.combined.txt") != std::string::npos);
    CHECK(preview.find("res-000001.raw_payload.json") != std::string::npos);
    std::ifstream file(audit.path.parent_path() / "artifacts" / "res-000001.raw_payload.json");
    Json raw;
    file >> raw;
    REQUIRE(raw.size() == 2);
    CHECK(raw[0].at("text") == first.text);
    CHECK(raw[1].at("text") == second.text);
    int durable_results = 0;
    for (const auto& row : audit.Rows()) if (row.value("type", "") == "message" && row.at("message").value("role", "") == "tool") {
        ++durable_results;
        CHECK(row.at("message").at("content") == preview);
    }
    CHECK(durable_results == 1);
}

TEST_CASE("B1 original capture survives post-tool hook feedback appended to the result") {
    Audit audit;
    AuditBackend backend;
    backend.emit = [](int attempt, const auto& sink) -> std::expected<void, api::Error> {
        Reply(sink, attempt == 1);
        return {};
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<AuditTool>();
    auto* counter = tool.get();
    const std::string original(2 * 1024 * 1024, 'x');
    tool->result_content = original;
    registry.Register(std::move(tool));
    agent::Agent agent(backend, registry, Profile());
    auto wiring = audit.Wiring();
    wiring.on_post_tool_use_hook = [](const std::string&, const std::string&, const Json&, const tools::Tool::Result&) {
        return std::vector<std::string>{"hook-added feedback"};
    };
    wiring.rewrite_tool_results_for_history = [&audit](api::Message& results) {
        return audit.bridge->RewriteToolResultsForHistory(results);
    };
    REQUIRE(agent.Run(Input(), wiring).has_value());
    CHECK(counter->calls == 1);
    REQUIRE(backend.requests.size() == 2);
    std::ifstream captured(audit.path.parent_path() / "artifacts" / "capture-000001.combined.txt", std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(captured), {}) == original);
    std::ifstream effective(audit.path.parent_path() / "artifacts" / "res-000001.combined.txt", std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(effective), {}) == original + "\n[post-tool-use hook 追加] hook-added feedback");
    const auto wire = api::chat::BuildRequestJson(backend.requests[1]);
    for (const auto& message : wire.at("messages")) if (message.value("role", "") == "tool")
        CHECK(message.at("content").get<std::string>().find("hook-added feedback") != std::string::npos);
    for (const auto& row : audit.Rows()) if (row.value("kind", "") == "tool.result.selected")
        CHECK(row.at("payload").at("sourceResultEventRefs").size() == 2);
    CHECK(v3::VerifyV3File(audit.path).ok);
}

TEST_CASE("B1 post-tool hook failure leaves the original capture recoverable") {
    Audit audit;
    AuditBackend backend;
    backend.emit = [](int attempt, const auto& sink) -> std::expected<void, api::Error> {
        Reply(sink, attempt == 1);
        return {};
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<AuditTool>();
    auto* counter = tool.get();
    const std::string original(2 * 1024 * 1024, 'x');
    tool->result_content = original;
    registry.Register(std::move(tool));
    agent::Agent agent(backend, registry, Profile());
    auto wiring = audit.Wiring();
    wiring.on_post_tool_hook = [](const std::string&, const std::string&, const Json&, const tools::Tool::Result&) {
        throw std::runtime_error("injected post-tool failure");
    };
    CHECK_THROWS_AS(agent.Run(Input(), wiring), std::runtime_error);
    CHECK(counter->calls == 1);
    CHECK(backend.requests.size() == 1);
    std::ifstream captured(audit.path.parent_path() / "artifacts" / "capture-000001.combined.txt", std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(captured), {}) == original);
    CHECK(KindCount(audit.Rows(), "tool.result.persisted") == 1);
    CHECK(KindCount(audit.Rows(), "tool.result.selected") == 0);
}

TEST_CASE("B1 original capture failure stops post-tool hooks and subsequent model sends") {
    Audit audit;
    AuditBackend backend;
    backend.emit = [](int attempt, const auto& sink) -> std::expected<void, api::Error> {
        Reply(sink, attempt <= 2, "audit-call-" + std::to_string(attempt));
        return {};
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<AuditTool>();
    auto* counter = tool.get();
    tool->result_content = std::string(2 * 1024 * 1024, 'x');
    tool->effect = [&audit, counter] {
        if (counter->calls == 2) {
            const auto destination = audit.path.parent_path() / "artifacts" / "capture-000002.json";
            std::filesystem::create_directories(destination);
            std::ofstream(destination / "keep") << "blocked";
        }
    };
    registry.Register(std::move(tool));
    agent::Agent agent(backend, registry, Profile());
    auto wiring = audit.Wiring();
    int post_hooks = 0;
    wiring.on_post_tool_hook = [&post_hooks](const std::string&, const std::string&, const Json&, const tools::Tool::Result&) {
        ++post_hooks;
    };
    wiring.rewrite_tool_results_for_history = [&audit](api::Message& results) {
        return audit.bridge->RewriteToolResultsForHistory(results);
    };
    CHECK_FALSE(agent.Run(Input(), wiring).has_value());
    CHECK(counter->calls == 2);
    CHECK(post_hooks == 1);
    CHECK(backend.requests.size() == 2);
    CHECK(KindCount(audit.Rows(), "tool.result.persist_failed") == 1);
    CHECK_FALSE(std::filesystem::exists(audit.path.parent_path() / "artifacts" / "res-000002.json"));
}

TEST_CASE("B2 real loop: media-bearing result ends the turn unestimated while captures persist") {
    // 产品决定(见 v3-action-summary.md「媒体边界」):文本 bytes/4 不给媒体
    // 定价,首发预算块遇到 Image/Audio/EmbeddedBlob 显式拒绝并终态,不静默
    // 放行。这条测试钉三件事:轮以 unestimated 错误收场、原始捕获(含图片
    // 块)完整落 artifacts、同批已执行的文本结果照常入账不丢。
    struct ImageTool final : tools::Tool {
        int calls = 0;
        std::string name() const override { return "image_tool"; }
        std::string description() const override { return "returns one image block"; }
        Json input_schema() const override { return Json::object(); }
        bool needs_confirm() const override { return false; }
        Result execute(const Json&) override {
            ++calls;
            tools::Tool::Result result{"screenshot captured", false};
            tools::ImageContent image;
            image.mime_type = "image/png";
            image.width = 4;
            image.height = 2;
            image.bytes = 8;
            image.sha256 = "media-sha";
            image.artifact.id = "art-mediasha";
            image.artifact.filename = "art-mediasha.png";
            image.artifact.path = "mcp-artifacts/art-mediasha.png";
            image.artifact.mime_type = "image/png";
            image.artifact.bytes = 8;
            image.artifact.sha256 = "media-sha";
            image.artifact.stored = true;
            tools::ToolResultPayload payload;
            payload.content.push_back(std::move(image));
            result.SetPayload(std::move(payload));
            return result;
        }
    };
    Audit audit;
    AuditBackend backend;
    backend.emit = [](int attempt, const auto& sink) -> std::expected<void, api::Error> {
        if (attempt != 1) {
            Reply(sink);
            return {};
        }
        sink(api::MessageStart{"provider-response", "audit-model"});
        sink(api::ToolUseStart{0, "media-text-1", "audit_tool"});
        sink(api::ToolUseInputDelta{0, "{}"});
        sink(api::ToolUseStart{1, "media-image-2", "image_tool"});
        sink(api::ToolUseInputDelta{1, "{}"});
        sink(api::ContentBlockDone{0});
        sink(api::ContentBlockDone{1});
        sink(api::MessageDone{"tool_use", api::Usage{}});
        return {};
    };
    tools::ToolRegistry registry;
    auto text_tool = std::make_unique<AuditTool>();
    auto* text_counter = text_tool.get();
    text_tool->result_content = "plain text evidence";
    registry.Register(std::move(text_tool));
    auto image_tool = std::make_unique<ImageTool>();
    auto* image_counter = image_tool.get();
    registry.Register(std::move(image_tool));
    agent::Agent agent(backend, registry, Profile());
    auto wiring = audit.Wiring();
    wiring.rewrite_tool_results_for_history = [&audit](api::Message& results) {
        return audit.bridge->RewriteToolResultsForHistory(results);
    };
    const auto outcome = agent.Run(Input(), wiring);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().find("tool_batch.unestimated_media_or_reasoning") != std::string::npos);
    CHECK(text_counter->calls == 1);
    CHECK(image_counter->calls == 1);
    CHECK(backend.requests.size() == 1);  // 终态后不发下一份请求
    const auto rows = audit.Rows();
    int raw_captures = 0;
    for (const auto& row : rows) {
        if (row.value("kind", "") != "tool.result.persisted") continue;
        for (const auto& ref : row.at("payload").at("result_ref")) {
            if (ref.value("path", "").find("capture-") != std::string::npos) {
                ++raw_captures;
                break;
            }
        }
    }
    CHECK(raw_captures == 2);  // 文本与图片两枚原始捕获都落了仓
    std::ifstream captured_text(audit.path.parent_path() / "artifacts" / "capture-000001.combined.txt",
                                std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(captured_text), {}) == "plain text evidence");
    std::ifstream captured_media(audit.path.parent_path() / "artifacts" / "capture-000002.raw_payload.json");
    Json media_blocks;
    captured_media >> media_blocks;
    REQUIRE(media_blocks.size() == 1);
    CHECK(media_blocks[0].at("type") == "image");
    CHECK(media_blocks[0].at("artifact").at("path") == "mcp-artifacts/art-mediasha.png");
    CHECK(media_blocks[0].at("artifact").at("stored") == true);
    // 同批文本结果照常入账:选用事件、有效正文与持久 tool 消息一枚不少。
    CHECK(KindCount(rows, "tool.result.selected") == 2);
    std::ifstream effective_text(audit.path.parent_path() / "artifacts" / "res-000001.combined.txt",
                                 std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(effective_text), {}) == "plain text evidence");
    int durable_results = 0;
    for (const auto& row : rows) {
        if (row.value("type", "") == "message" && row.at("message").value("role", "") == "tool") ++durable_results;
    }
    CHECK(durable_results == 2);
    CHECK(v3::VerifyV3File(audit.path).ok);
}
