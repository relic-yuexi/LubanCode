#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fake_http_server.hpp"
#include "lubancore/core.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "runtime/session_service.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"
#include "workspace/index.hpp"

namespace {
using namespace std::chrono_literals;
namespace sdk = lubancore;
namespace fs = std::filesystem;
namespace platform = lubancode::platform;
using Json = nlohmann::json;

struct LifecycleFixture {
    fs::path root;
    LifecycleFixture() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("lubancore-lifecycle-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(++serial));
        fs::create_directories(root / "cwd");
        fs::create_directories(root / "resources");
    }
    ~LifecycleFixture() { std::error_code ec; fs::remove_all(root, ec); }
    sdk::RuntimeOptions Roots() const {
        return {platform::PathToUtf8(root / "data"), platform::PathToUtf8(root / "resources")};
    }
    fs::path SessionDir(const std::string& id) const {
        auto identity = lubancode::workspace::ResolveWorkspaceIdentity(root / "cwd", root / "data");
        REQUIRE(identity.has_value());
        auto workspace_dir = lubancode::workspace::index::ResolveDirByWorkspaceKey(
            root / "data" / "workspaces", identity->workspace_key);
        REQUIRE(workspace_dir.has_value());
        return *workspace_dir / "sessions" / id;
    }
    lubancode::runtime::SessionLaunchRequest RawLaunch() const {
        lubancode::runtime::SessionLaunchRequest launch;
        launch.cwd_utf8 = platform::PathToUtf8(root / "cwd");
        auto identity = lubancode::workspace::ResolveWorkspaceIdentity(root / "cwd", root / "data");
        REQUIRE(identity.has_value());
        launch.workspace_identity = *identity;
        launch.workspaces_root = root / "data" / "workspaces";
        launch.lubancode_version = "sdk-lifecycle-test";
        launch.v3_system_content = "SDK lifecycle fixture";
        return launch;
    }
};

using GenerateFunction = std::function<sdk::Result<sdk::ModelReply>(const sdk::ModelRequest&, sdk::Cancellation)>;
class CallbackBackend final : public sdk::Backend {
public:
    explicit CallbackBackend(GenerateFunction generate) : generate_(std::move(generate)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancel) override {
        return generate_(request, cancel);
    }
private:
    GenerateFunction generate_;
};
sdk::SessionOptions Options(const LifecycleFixture& fixture, GenerateFunction generate) {
    sdk::SessionOptions options;
    options.cwd = platform::PathToUtf8(fixture.root / "cwd");
    options.model = "fixture";
    options.system_prompt = "SDK lifecycle fixture";
    options.max_steps_per_turn = 4;
    options.backend = std::make_unique<CallbackBackend>(std::move(generate));
    return options;
}

lubancode::test_support::FakeHttpResponse Sse(std::vector<std::string> frames) {
    lubancode::test_support::FakeHttpResponse response;
    response.headers.emplace_back("Content-Type", "text/event-stream");
    for (const auto& frame : frames) response.body += "data: " + frame + "\n\n";
    return response;
}
} // namespace

TEST_CASE("SDK lifecycle: unrecoverable accepted input fails resume instead of waiting forever") {
    LifecycleFixture fixture;
    std::string session_id, operation_id;
    {
        lubancode::runtime::SessionService source(fixture.RawLaunch());
        REQUIRE(source.runtime() != nullptr);
        session_id = source.trajectory()->session_id();
        auto receipt = source.SubmitInput({"accepted-key", "must not disappear", {}});
        REQUIRE(receipt.accepted);
        operation_id = receipt.operation_id;
        REQUIRE(source.Close("fixture_checkpoint").error_code.empty());
    }
    const auto input = fixture.SessionDir(session_id) / "operations-inputs" / (operation_id + ".json");
    SUBCASE("missing original") { REQUIRE(fs::remove(input)); }
    SUBCASE("malformed original") { std::ofstream(input, std::ios::trunc) << "{broken"; }
    SUBCASE("valid JSON with changed payload") {
        Json original;
        { std::ifstream file(input); file >> original; }
        original["text"] = "changed after durable acceptance";
        std::ofstream(input, std::ios::trunc) << original.dump();
    }

    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto options = Options(fixture, [calls](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        ++*calls;
        return sdk::ModelReply{"should not execute"};
    });
    options.resume_session_id = session_id;
    auto resumed = (*runtime)->OpenSession(std::move(options));
    REQUIRE_FALSE(resumed.has_value());
    CHECK(resumed.error().code == "sdk.resume.input_unavailable");
    CHECK(calls->load() == 0);
    REQUIRE((*runtime)->Shutdown().has_value());
    // Failed opening must release its source lock and file handles as well.
    fs::rename(fixture.root / "data", fixture.root / "closed-data");
}

TEST_CASE("SDK lifecycle: corrupt operation ledger cannot discard a completed operation identity") {
    LifecycleFixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto first = (*runtime)->OpenSession(Options(fixture, [](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        return sdk::ModelReply{"durable completed answer"};
    }));
    REQUIRE(first.has_value());
    const auto session_id = (*first)->id();
    const auto submitted = (*first)->Submit("durable-original-key", "completed input");
    REQUIRE(submitted.has_value());
    const auto completed = (*first)->WaitResult(submitted->operation_id, 15s);
    REQUIRE(completed.has_value());
    REQUIRE(completed->state == sdk::OperationState::Succeeded);
    REQUIRE(completed->result_persisted);
    REQUIRE((*first)->Close().has_value());

    const auto source_dir = fixture.SessionDir(session_id);
    const auto operations_file = source_dir / "operations.jsonl";
    std::vector<std::string> lines;
    {
        std::ifstream input(operations_file, std::ios::binary);
        for (std::string line; std::getline(input, line);) lines.push_back(std::move(line));
    }
    REQUIRE(lines.size() == 3);
    REQUIRE(Json::parse(lines[0]).value("kind", "") == "operation.accepted");
    REQUIRE(Json::parse(lines[1]).value("kind", "") == "operation.dispatched");
    REQUIRE(Json::parse(lines[2]).value("kind", "") == "operation.final");
    bool rewrite_ledger = true;
    SUBCASE("malformed accepted line") { lines[0] = "{broken accepted fact"; }
    SUBCASE("accepted line missing but following facts remain valid JSON") { lines.erase(lines.begin()); }
    SUBCASE("conflicting second acceptance for the same operation") {
        auto conflict = Json::parse(lines[0]);
        conflict["clientOperationId"] = "different-client-key";
        lines.insert(lines.begin() + 1, conflict.dump());
    }
    SUBCASE("whole ledger missing while completed result remains") {
        REQUIRE(fs::remove(operations_file));
        rewrite_ledger = false;
    }
    SUBCASE("whole ledger empty while completed result remains") { lines.clear(); }
    if (rewrite_ledger) {
        std::ofstream output(operations_file, std::ios::binary | std::ios::trunc);
        for (const auto& line : lines) output << line << '\n';
        output.close();
        REQUIRE_FALSE(output.fail());
    }
    // Only the operation ledger was changed. A valid V3 stream alone cannot
    // prove that operation keys and counters remain safe to reuse.
    REQUIRE(lubancode::trajectory::v3::ReadV3Ledger(source_dir / (session_id + ".jsonl")).has_value());
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto options = Options(fixture, [calls](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        ++*calls;
        return sdk::ModelReply{"must not execute"};
    });
    options.resume_session_id = session_id;
    const auto resumed = (*runtime)->OpenSession(std::move(options));
    REQUIRE_FALSE(resumed.has_value());
    CHECK(resumed.error().code == "sdk.resume.operation_ledger_invalid");
    CHECK(calls->load() == 0);
    REQUIRE((*runtime)->Shutdown().has_value());
    fs::rename(fixture.root / "data", fixture.root / "closed-data");
}

TEST_CASE("SDK lifecycle: custom tool cannot join another session or shut down its runtime") {
    LifecycleFixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto idle = (*runtime)->OpenSession(Options(fixture, [](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        return sdk::ModelReply{"idle answer"};
    }));
    REQUIRE(idle.has_value());
    auto steps = std::make_shared<std::atomic<int>>(0);
    auto checked = std::make_shared<std::atomic<bool>>(false);
    auto options = Options(fixture, [steps](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        if (steps->fetch_add(1) == 0) return sdk::ModelReply{"", {{"reentrant-call", "probe", "{}"}}};
        return sdk::ModelReply{"callback returned"};
    });
    sdk::Tool probe;
    probe.name = "probe";
    probe.requires_approval = false;
    probe.execute = [other = *idle, owner = runtime->get(), checked](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
        const auto close = other->Close();
        const auto wait = other->WaitResult("unused", 1ms);
        const auto shutdown = owner->Shutdown();
        checked->store(!close && close.error().code == "sdk.lifecycle.reentrant" &&
                       !wait && wait.error().code == "sdk.lifecycle.reentrant" &&
                       !shutdown && shutdown.error().code == "sdk.lifecycle.reentrant");
        return sdk::ToolResult{"checked"};
    };
    options.custom_tools.push_back(std::move(probe));
    auto session = (*runtime)->OpenSession(std::move(options));
    REQUIRE(session.has_value());
    auto submitted = (*session)->Submit("tool-key", "exercise tool callback");
    REQUIRE(submitted.has_value());
    auto result = (*session)->WaitResult(submitted->operation_id, 15s);
    REQUIRE(result.has_value());
    CHECK(result->state == sdk::OperationState::Succeeded);
    CHECK(checked->load());
    // Rejected lifecycle calls must not close admission in either handle.
    auto still_open = (*idle)->Submit("after-callback", "still open");
    REQUIRE(still_open.has_value());
    REQUIRE((*idle)->WaitResult(still_open->operation_id, 15s).has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK lifecycle: resumed session rejects an approval token from its previous turn") {
    LifecycleFixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto executions = std::make_shared<std::atomic<int>>(0);
    const auto make_options = [&] {
        auto calls = std::make_shared<std::atomic<int>>(0);
        auto options = Options(fixture, [calls](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
            if (calls->fetch_add(1) == 0) return sdk::ModelReply{"", {{"guarded-call", "guarded", "{}"}}};
            return sdk::ModelReply{"approved current turn"};
        });
        sdk::Tool tool;
        tool.name = "guarded";
        tool.execute = [executions](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
            ++*executions;
            return sdk::ToolResult{"executed"};
        };
        options.custom_tools.push_back(std::move(tool));
        return options;
    };
    const auto await_approval = [](const std::shared_ptr<sdk::EventStream>& events) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            auto event = events->Next(100ms);
            REQUIRE(event.has_value());
            if (event->has_value() && (*event)->approval) return *(*event)->approval;
        }
        FAIL("expected approval event did not arrive");
        return sdk::Approval{};
    };

    auto first = (*runtime)->OpenSession(make_options());
    REQUIRE(first.has_value());
    const auto session_id = (*first)->id();
    auto first_events = (*first)->Subscribe();
    REQUIRE(first_events.has_value());
    const auto first_receipt = (*first)->Submit("old-turn", "await old approval");
    REQUIRE(first_receipt.has_value());
    const auto old_approval = await_approval(*first_events);
    REQUIRE_FALSE(old_approval.request_id.empty());
    REQUIRE((*first)->Close().has_value());
    const auto old_result = (*first)->ReadOperation(first_receipt->operation_id);
    REQUIRE(old_result.has_value());
    CHECK(old_result->state == sdk::OperationState::Cancelled);
    CHECK(executions->load() == 0);

    auto resume_options = make_options();
    resume_options.resume_session_id = session_id;
    auto resumed = (*runtime)->OpenSession(std::move(resume_options));
    REQUIRE(resumed.has_value());
    CHECK((*resumed)->id() == session_id);
    auto new_events = (*resumed)->Subscribe();
    REQUIRE(new_events.has_value());
    const auto new_receipt = (*resumed)->Submit("new-turn", "await current approval");
    REQUIRE(new_receipt.has_value());
    const auto current_approval = await_approval(*new_events);
    CHECK(current_approval.request_id != old_approval.request_id);
    const auto stale = (*resumed)->ResolveApproval(old_approval.request_id, sdk::ApprovalDecision::Accept);
    REQUIRE_FALSE(stale.has_value());
    CHECK(stale.error().code == "stale_request_id");
    const auto still_pending = (*resumed)->PendingApprovals();
    REQUIRE(still_pending.size() == 1);
    CHECK(still_pending.front().request_id == current_approval.request_id);
    CHECK(executions->load() == 0);

    REQUIRE((*resumed)->ResolveApproval(current_approval.request_id, sdk::ApprovalDecision::Accept).has_value());
    const auto result = (*resumed)->WaitResult(new_receipt->operation_id, 15s);
    REQUIRE(result.has_value());
    CHECK(result->state == sdk::OperationState::Succeeded);
    CHECK(result->result_persisted);
    CHECK(result->turn_id != old_result->turn_id);
    CHECK(executions->load() == 1);
    REQUIRE((*runtime)->Shutdown().has_value());
}

namespace {
void CheckMcpResultBoundary(bool image) {
    LifecycleFixture fixture;
#ifdef _WIN32
    const char* python = "python";
#else
    const char* python = "python3";
#endif
    const auto located = platform::RunProcess({python, "-c", "import json,sys; print(json.dumps(sys.executable))"}, 10000);
    REQUIRE_FALSE(located.spawn_failed);
    REQUIRE_FALSE(located.timed_out);
    REQUIRE(located.exit_code == 0);
    const auto executable = Json::parse(located.output);
    REQUIRE(executable.is_string());
    REQUIRE(platform::Utf8ToPath(executable.get<std::string>()).is_absolute());

    lubancode::test_support::FakeHttpServer server;
    const auto arguments = Json{{"kind", image ? "image" : "text"}}.dump();
    const auto argument_delta = Json{{"type", "content_block_delta"}, {"index", 0},
        {"delta", {{"type", "input_json_delta"}, {"partial_json", arguments}}}}.dump();
    server.Enqueue(Sse({
        R"({"type":"message_start","message":{"id":"m1","model":"fixture","usage":{"input_tokens":1,"output_tokens":0}}})",
        R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"rich-call","name":"mcp__fixture__rich","input":{}}})",
        argument_delta,
        R"({"type":"content_block_stop","index":0})",
        R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":1}})",
        R"({"type":"message_stop"})"}));
    server.Enqueue(Sse({
        R"({"type":"message_start","message":{"id":"m2","model":"fixture","usage":{"input_tokens":1,"output_tokens":0}}})",
        R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"text received"}})",
        R"({"type":"content_block_stop","index":0})",
        R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":1}})",
        R"({"type":"message_stop"})"}));

    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    sdk::SessionOptions options;
    options.cwd = platform::PathToUtf8(fixture.root / "cwd");
    options.model = "fixture";
    options.connection = sdk::Connection{sdk::Wire::Anthropic,
        "http://127.0.0.1:" + std::to_string(server.port()), "FAKE_SDK_FIXTURE"};
    options.approval_mode = sdk::ApprovalMode::Yolo;
    options.max_steps_per_turn = 4;
    sdk::McpServer mcp;
    mcp.name = "fixture";
    mcp.command = executable.get<std::string>();
    mcp.arguments = {std::string(LUBANCODE_TEST_FIXTURES_DIR) + "/mcp_test_server.py"};
    for (const char* name : {"SystemRoot", "PATH", "TEMP", "TMP"}) {
        if (const auto value = platform::GetEnvVar(name)) mcp.environment.emplace_back(name, *value);
    }
    mcp.tools = {"rich"};
    options.mcp_servers.push_back(std::move(mcp));
    auto session = (*runtime)->OpenSession(std::move(options));
    REQUIRE(session.has_value());
    const auto session_id = (*session)->id();
    auto events = (*session)->Subscribe();
    REQUIRE(events.has_value());
    const auto submitted = (*session)->Submit("mcp-key", "inspect fixture result");
    REQUIRE(submitted.has_value());
    const auto result = (*session)->WaitResult(submitted->operation_id, 20s);
    REQUIRE(result.has_value());
    std::string observed_events;
    for (;;) {
        const auto event = (*events)->Next(0ms);
        if (!event) { observed_events += event.error().code; break; }
        if (!event->has_value()) break;
        observed_events += (*event)->kind + ";";
    }
    INFO("MCP operation error: " << result->error);
    INFO("MCP event sequence (kinds only): " << observed_events);
    const auto session_dir = fixture.SessionDir(session_id);
    INFO("MCP session directory: " << platform::PathToUtf8(session_dir));
    INFO("MCP session path characters: " << session_dir.native().size());
    INFO("MCP PNG path characters before atomic temporary suffix: " <<
         (session_dir / "artifacts" / "sha256" / (std::string(64, 'a') + ".png")).native().size());
    std::string unexpected_tool_result;
    if (image) {
        const auto captured_requests = server.requests();
        if (captured_requests.size() > 1) {
            const auto request = Json::parse(captured_requests[1].body);
            for (const auto& message : request.at("messages")) {
                for (const auto& block : message.at("content")) {
                    if (block.value("type", "") != "tool_result" ||
                        block.value("tool_use_id", "") != "rich-call") continue;
                    // Only this fixture's tool reply is shown, never request
                    // headers, connection settings or the whole model request.
                    unexpected_tool_result += "is_error=" + std::string(block.value("is_error", false) ? "true" : "false") +
                        " content=" + block.at("content").dump().substr(0, 1200);
                }
            }
        }
    }
    INFO("Unexpected image follow-up tool reply: " << unexpected_tool_result);
    CHECK(result->result_persisted);
    if (image) {
        // The current production media-budget contract rejects unestimated
        // image/audio/blob results after capture. It must not invent a token
        // price, rerun the tool, or send the next request.
        CHECK(result->state == sdk::OperationState::Failed);
        CHECK(result->error.find("tool_batch.unestimated_media_or_reasoning") != std::string::npos);
        CHECK(result->final_text.empty());
    } else {
        CHECK(result->state == sdk::OperationState::Succeeded);
        CHECK(result->error.empty());
        CHECK(result->final_text == "text received");
    }
    const auto duplicate = (*session)->Submit("mcp-key", "inspect fixture result");
    REQUIRE(duplicate.has_value());
    CHECK(duplicate->duplicate);
    CHECK(duplicate->operation_id == submitted->operation_id);
    REQUIRE((*session)->Close().has_value());

    const auto requests = server.requests();
    REQUIRE(requests.size() == (image ? 1 : 2));
    CHECK(requests[0].target == "/v1/messages");
    if (!image) {
        CHECK(requests[1].target == "/v1/messages");
        const auto request = Json::parse(requests[1].body);
        bool model_received_text = false;
        for (const auto& message : request.at("messages")) {
            for (const auto& block : message.at("content")) {
                if (block.value("type", "") != "tool_result" ||
                    block.value("tool_use_id", "") != "rich-call") continue;
                CHECK_FALSE(block.value("is_error", false));
                REQUIRE(block.at("content").is_string());
                model_received_text = block.at("content").get<std::string>().find("只有文本") != std::string::npos;
            }
        }
        CHECK(model_received_text);
    }

    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(session_dir / (session_id + ".jsonl"));
    REQUIRE(ledger.has_value());
    using Kind = lubancode::trajectory::v3::EventKindV3;
    std::string action_id;
    for (const auto& event : ledger->events) {
        if (event.kind == Kind::ToolExecutionPending &&
            event.payload.value("provider_tool_call_id", "") == "rich-call") {
            REQUIRE(action_id.empty());
            REQUIRE(event.action_id.has_value());
            action_id = *event.action_id;
        }
    }
    REQUIRE_FALSE(action_id.empty());
    int starts = 0, finishes = 0, persisted = 0, image_captures = 0;
    for (const auto& event : ledger->events) {
        if (event.kind == Kind::ToolExecutionStarted) {
            ++starts;
            CHECK(event.action_id == action_id);
        }
        if (event.kind == Kind::ToolExecutionFinished) {
            ++finishes;
            CHECK(event.action_id == action_id);
        }
        if (event.kind != Kind::ToolResultPersisted) continue;
        ++persisted;
        CHECK(event.action_id == action_id);
        CHECK(event.payload.value("tool_call_id", "") == action_id);
        if (!image) continue;
        for (const auto& ref : event.payload.at("result_ref")) {
            const auto relative_path = ref.value("path", "");
            if (ref.value("kind", "") != "raw_payload" || relative_path.find("capture-") == std::string::npos) continue;
            ++image_captures;
            std::ifstream captured(session_dir / platform::Utf8ToPath(relative_path), std::ios::binary);
            REQUIRE(captured.is_open());
            const auto blocks = Json::parse(captured);
            REQUIRE(blocks.size() == 2);
            CHECK(blocks[0].value("type", "") == "text");
            CHECK(blocks[1].value("type", "") == "image");
            CHECK(blocks[1].value("mime_type", "") == "image/png");
            const auto& artifact = blocks[1].at("artifact");
            CHECK(artifact.value("stored", false));
            auto png = platform::Utf8ToPath(artifact.at("path").get<std::string>());
            if (png.is_relative()) png = session_dir / png;
            REQUIRE(fs::is_regular_file(png));
            CHECK(fs::equivalent(png.parent_path(), session_dir / "artifacts" / "sha256"));
            CHECK(png.extension() == ".png");
            CHECK(fs::file_size(png) > 0);
        }
    }
    CHECK(starts == 1);
    CHECK(finishes == 1);
    CHECK(persisted >= 1);
    if (image) CHECK(image_captures == 1);
}
} // namespace

TEST_CASE("SDK lifecycle: real MCP text reaches the next model request and completes once") {
    CheckMcpResultBoundary(false);
}

TEST_CASE("SDK lifecycle: real MCP image is captured once and rejected by the media budget") {
    CheckMcpResultBoundary(true);
}
