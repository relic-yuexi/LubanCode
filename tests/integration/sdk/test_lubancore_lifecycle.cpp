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
        return root / "data" / "workspaces" / identity->workspace_key / "sessions" / id;
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

TEST_CASE("SDK lifecycle: real MCP image persists in the session and reaches the next model request") {
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
    server.Enqueue(Sse({
        R"({"type":"message_start","message":{"id":"m1","model":"fixture","usage":{"input_tokens":1,"output_tokens":0}}})",
        R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"image-call","name":"mcp__fixture__rich","input":{}}})",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"kind\":\"image\"}"}})",
        R"({"type":"content_block_stop","index":0})",
        R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":1}})",
        R"({"type":"message_stop"})"}));
    server.Enqueue(Sse({
        R"({"type":"message_start","message":{"id":"m2","model":"fixture","usage":{"input_tokens":1,"output_tokens":0}}})",
        R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"image persisted"}})",
        R"({"type":"content_block_stop","index":0})",
        R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":1}})",
        R"({"type":"message_stop"})"}));

    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    sdk::SessionOptions options;
    options.cwd = platform::PathToUtf8(fixture.root / "cwd");
    options.model = "fixture";
    options.connection = sdk::Connection{sdk::Wire::Anthropic,
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1", "FAKE_SDK_FIXTURE"};
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
    const auto submitted = (*session)->Submit("image-key", "inspect fixture image");
    REQUIRE(submitted.has_value());
    const auto result = (*session)->WaitResult(submitted->operation_id, 20s);
    REQUIRE(result.has_value());
    CHECK(result->state == sdk::OperationState::Succeeded);
    CHECK(result->result_persisted);
    CHECK(result->final_text == "image persisted");
    REQUIRE((*session)->Close().has_value());

    const auto requests = server.requests();
    REQUIRE(requests.size() == 2);
    // The base64 must come from rehydrating the stored image, not a path-only
    // or error placeholder passed to the next model request.
    const auto request = Json::parse(requests[1].body);
    bool model_received_image = false;
    for (const auto& message : request.at("messages")) {
        for (const auto& block : message.at("content")) {
            if (block.value("type", "") != "tool_result" || !block.at("content").is_array()) continue;
            for (const auto& content : block.at("content")) {
                if (content.value("type", "") != "image") continue;
                const auto& source = content.at("source");
                model_received_image = source.value("type", "") == "base64" &&
                    source.value("media_type", "") == "image/png" &&
                    source.value("data", "").starts_with("iVBORw0KGgo");
            }
        }
    }
    CHECK(model_received_image);
    const auto artifacts = fixture.SessionDir(session_id) / "artifacts" / "sha256";
    REQUIRE(fs::is_directory(artifacts));
    bool image_found = false;
    for (const auto& entry : fs::directory_iterator(artifacts)) {
        if (entry.path().extension() == ".png" && fs::file_size(entry.path()) > 0) image_found = true;
    }
    CHECK(image_found);
    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(fixture.SessionDir(session_id) / (session_id + ".jsonl"));
    REQUIRE(ledger.has_value());
    bool recorded_image = false;
    for (const auto& message : ledger->messages) {
        if (message.message.dump().find("image_ref") != std::string::npos) recorded_image = true;
    }
    CHECK(recorded_image);
}
