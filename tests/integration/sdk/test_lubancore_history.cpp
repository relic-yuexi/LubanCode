#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>

#include "lubancore/core.hpp"
#include "mcp_cwd_fixture.hpp"
#include "session_history_fixture.hpp"

namespace {
namespace sdk = lubancore;
namespace history = lubancode::test_support::session_history;
using namespace std::chrono_literals;

sdk::SessionOptions Options(const history::Fixture& fixture, std::atomic<int>& calls) {
    sdk::SessionOptions options;
    options.cwd = fixture.Cwd();
    options.model = "history-model";
    options.system_prompt = "Preserve session history.";
    options.connection = sdk::Connection{sdk::Wire::ChatCompletions, fixture.Url(), "FAKE_HISTORY_KEY"};
    options.max_steps_per_turn = 4;
    sdk::Tool tool;
    tool.name = history::kTool;
    tool.description = "Return a session history marker.";
    tool.input_schema_json = R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})";
    tool.requires_approval = false;
    tool.execute = [&calls](const std::string& input, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
        CHECK(history::json::parse(input) == history::json({{"text", history::kArgument}}));
        ++calls;
        return sdk::ToolResult{history::kToolResult, false};
    };
    options.custom_tools.push_back(std::move(tool));
    return options;
}

void Turn(const std::shared_ptr<sdk::Session>& session, const char* input, const char* answer) {
    auto receipt = session->Submit(input, input);
    REQUIRE(receipt.has_value());
    auto result = session->WaitResult(receipt->operation_id, 30s);
    REQUIRE(result.has_value());
    INFO(result->error);
    CHECK(result->state == sdk::OperationState::Succeeded);
    CHECK(result->result_persisted);
    CHECK(result->final_text == answer);
}
}  // namespace

TEST_CASE("SDK history: HTTP requests preserve two turns and same-ID resume without shared-cwd leakage") {
    history::Fixture fixture;
    std::atomic<int> calls{0};
    std::string id;
    const sdk::RuntimeOptions roots{history::Utf8(fixture.root / "data"),
                                    history::Utf8(fixture.root / "resources")};
    {
        auto runtime = sdk::Runtime::Create(roots);
        REQUIRE(runtime.has_value());
        auto first = (*runtime)->OpenSession(Options(fixture, calls));
        REQUIRE(first.has_value());
        id = (*first)->id();
        Turn(*first, history::kFirst, history::kFirstAnswer);
        Turn(*first, history::kSecond, history::kSecondAnswer);
        auto other = (*runtime)->OpenSession(Options(fixture, calls));
        REQUIRE(other.has_value());
        CHECK((*other)->id() != id);
        Turn(*other, history::kOther, history::kOtherAnswer);
        REQUIRE((*runtime)->Shutdown().has_value());
    }
    {
        auto runtime = sdk::Runtime::Create(roots);
        REQUIRE(runtime.has_value());
        auto options = Options(fixture, calls);
        options.resume_session_id = id;
        auto resumed = (*runtime)->OpenSession(std::move(options));
        REQUIRE(resumed.has_value());
        CHECK((*resumed)->id() == id);
        Turn(*resumed, history::kThird, history::kThirdAnswer);
        REQUIRE((*runtime)->Shutdown().has_value());
    }
    CHECK(calls.load() == 1);
    history::CheckRequests(fixture);
}

TEST_CASE("SDK resources: live MCP children follow session cwd and survive another session closing") {
    lubancode::test_support::McpCwdFixture fixture;
    auto runtime = sdk::Runtime::Create({history::Utf8(fixture.root / "data"),
                                        history::Utf8(fixture.root / "resources")});
    REQUIRE(runtime.has_value());
    std::vector<std::shared_ptr<sdk::Session>> sessions;
    for (int index = 0; index != 3; ++index) {
        sdk::SessionOptions options;
        options.cwd = history::Utf8(fixture.root / (index == 2 ? "project-b" : "project-a"));
        options.model = "cwd-model";
        options.connection = sdk::Connection{sdk::Wire::ChatCompletions, fixture.Url(), "FAKE_CWD_KEY"};
        options.approval_mode = sdk::ApprovalMode::Yolo;
        options.max_steps_per_turn = 4;
        sdk::McpServer mcp;
        mcp.name = "location";
        mcp.command = fixture.python;
        mcp.arguments = {fixture.Script()};
        mcp.tools = {"where"};
        for (const char* key : {"SystemRoot", "PATH", "TEMP", "TMP"}) {
            if (auto value = lubancode::platform::GetEnvVar(key)) mcp.environment.emplace_back(key, *value);
        }
        options.mcp_servers.push_back(std::move(mcp));
        auto session = (*runtime)->OpenSession(std::move(options));
        REQUIRE(session.has_value());
        sessions.push_back(*session);
        CHECK(std::filesystem::equivalent(fixture.host_cwd, std::filesystem::current_path()));
    }
    for (int round = 0; round != 3; ++round) {
        const auto input = fixture.Input(round);
        Turn(sessions[round], input.c_str(), "cwd-answer");
    }
    REQUIRE(sessions[0]->Close().has_value());
    sessions[0].reset();
    const auto input = fixture.Input(3);
    Turn(sessions[1], input.c_str(), "cwd-answer");
    REQUIRE((*runtime)->Shutdown().has_value());
    fixture.Check();
}
