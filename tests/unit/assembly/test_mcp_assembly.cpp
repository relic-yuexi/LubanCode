#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mcp/mcp_tool.hpp"
#include "runtime/assembly/mcp.hpp"
#include "scripted_mcp_endpoint.hpp"

namespace {
namespace assembly = lubancode::runtime::assembly;
using lubancode::test_support::ScriptedMcpEndpoint;

assembly::McpLaunchRequest Request(std::string name = "fixture") {
    return {std::move(name), "unused-command", {"arg"}, {{"FIXTURE_ENV", "fake-value"}},
            lubancode::platform::EnvMode::Replace};
}
}  // namespace

TEST_CASE("MCP assembly: one owner performs initialize then discovery without registering tools") {
    ScriptedMcpEndpoint endpoint;
    auto result = assembly::StartMcpServer(Request(), {}, endpoint.Launcher());
    REQUIRE(result.has_value());
    CHECK(result->name == "fixture");
    CHECK(result->client->Alive());
    CHECK(result->client->negotiated_protocol_version() == "2024-11-05");
    REQUIRE(result->tools.size() == 2);
    CHECK(result->tools[0].name == "zulu"); // 发现序原样交宿主，由宿主选取/排序。
    CHECK(result->tools[1].name == "echo");
    CHECK_FALSE(result->package_origin.has_value());
    CHECK(endpoint.methods == std::vector<std::string>{"initialize", "notifications/initialized", "tools/list"});
}

TEST_CASE("MCP assembly: launch parameters are explicit and start failure has no partial owner") {
    int launches = 0;
    auto request = Request();
    request.cwd_utf8 = "explicit-session-cwd";
    const auto result = assembly::StartMcpServer(request, {}, [&](const auto& actual)
        -> std::expected<std::unique_ptr<lubancode::mcp::Client>, std::string> {
        ++launches;
        CHECK(actual.name == request.name);
        CHECK(actual.command == request.command);
        CHECK(actual.args == request.args);
        CHECK(actual.env == request.env);
        CHECK(actual.env_mode == lubancode::platform::EnvMode::Replace);
        CHECK(actual.cwd_utf8 == request.cwd_utf8);
        return std::unexpected("fixture launch failed");
    });
    REQUIRE_FALSE(result.has_value());
    CHECK(launches == 1);
    CHECK(result.error().stage == assembly::McpStartupStage::Start);
    CHECK(result.error().error == "fixture launch failed");
}

TEST_CASE("MCP assembly: failed initialize or discovery closes the candidate exactly once") {
    for (const std::string method : {"initialize", "tools/list"}) {
        CAPTURE(method);
        ScriptedMcpEndpoint endpoint;
        endpoint.fail_method = method;
        const auto result = assembly::StartMcpServer(Request(), {}, endpoint.Launcher());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().stage == (method == "initialize" ? assembly::McpStartupStage::Initialize
                                                              : assembly::McpStartupStage::Discover));
        CHECK(result.error().error.find("fixture failure") != std::string::npos);
        CHECK(endpoint.shutdown_count == 1);
        CHECK_FALSE(endpoint.IsAlive());
        if (method == "initialize") {
            CHECK(endpoint.methods == std::vector<std::string>{"initialize"});
        }
    }
}

TEST_CASE("MCP assembly: a launcher cannot publish an empty successful Client") {
    const auto result = assembly::StartMcpServer(Request(), {}, [](const auto&)
        -> std::expected<std::unique_ptr<lubancode::mcp::Client>, std::string> {
        return std::unique_ptr<lubancode::mcp::Client>{};
    });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().stage == assembly::McpStartupStage::Start);
}

TEST_CASE("MCP assembly: explicit timeout bounds both protocol stages and closes candidates") {
    for (const std::string method : {"initialize", "tools/list"}) {
        CAPTURE(method);
        ScriptedMcpEndpoint endpoint;
        endpoint.silent_method = method;
        const auto before = std::chrono::steady_clock::now();
        const auto result = assembly::StartMcpServer(Request(), {25, 0, nullptr}, endpoint.Launcher());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error.find("超时") != std::string::npos);
        CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(2));
        CHECK(endpoint.shutdown_count == 1);
    }
}

TEST_CASE("MCP assembly: cancellation before launch has zero startup side effects") {
    std::atomic<bool> cancel{true};
    int launches = 0;
    auto result = assembly::StartMcpServer(Request(), {0, 0, &cancel}, [&](const auto&)
        -> std::expected<std::unique_ptr<lubancode::mcp::Client>, std::string> {
        ++launches;
        return std::unexpected("must not launch");
    });
    REQUIRE_FALSE(result.has_value());
    CHECK(launches == 0);
    CHECK(result.error().stage == assembly::McpStartupStage::Start);
}

TEST_CASE("MCP assembly: cancellation reaches the outstanding initialize or discovery request") {
    for (const std::string method : {"initialize", "tools/list"}) {
        CAPTURE(method);
        ScriptedMcpEndpoint endpoint;
        std::atomic<bool> cancel{false};
        endpoint.silent_method = method;
        endpoint.on_method = [&](const std::string& sent) { if (sent == method) cancel.store(true); };
        const auto result = assembly::StartMcpServer(Request(), {500, 0, &cancel}, endpoint.Launcher());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error.find("fixture cancelled") != std::string::npos);
        CHECK(std::find(endpoint.methods.begin(), endpoint.methods.end(), "notifications/cancelled") != endpoint.methods.end());
        CHECK(endpoint.shutdown_count == 1);
    }
}

TEST_CASE("MCP assembly: moving owners preserves the Client borrowed by registered tools") {
    ScriptedMcpEndpoint endpoint;
    std::vector<assembly::McpServerRuntime> owners;
    auto started = assembly::StartMcpServer(Request(), {}, endpoint.Launcher());
    REQUIRE(started.has_value());
    owners.push_back(std::move(*started));
    auto* client = owners.front().client.get();
    {
        lubancode::tools::ToolRegistry registry;
        registry.Register(std::make_unique<lubancode::mcp::McpTool>(*client, "fixture", owners.front().tools[1]));
        // 确保 vector 真重分配；Client 的堆地址须保持。
        owners.reserve(owners.capacity() + 32);
        CHECK(owners.front().client.get() == client);
        auto* tool = registry.Find("mcp__fixture__echo");
        REQUIRE(tool != nullptr);
        const auto result = tool->execute(nlohmann::json::object());
        CHECK_FALSE(result.is_error);
        CHECK(result.content == "fixture ok");
        CHECK(endpoint.IsAlive());
    }
    owners.clear();
    CHECK(endpoint.shutdown_count == 1);
}
