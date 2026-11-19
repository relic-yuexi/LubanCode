#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "mcp/mcp_tool.hpp"
#include "runtime/assembly/session_resources.hpp"
#include "scripted_mcp_endpoint.hpp"
#include "tools/path_utils.hpp"

namespace {
using namespace lubancode;
namespace assembly = runtime::assembly;
using test_support::ScriptedMcpEndpoint;
static_assert(!std::is_move_constructible_v<assembly::SessionResources>);
static_assert(!std::is_move_assignable_v<assembly::SessionResources>);

class BackendProbe : public api::Backend {
public:
    explicit BackendProbe(std::vector<std::string>& events) : events_(events) {}
    ~BackendProbe() override { events_.push_back("backend"); }
    std::expected<void, api::Error> send_stream(const api::Request&,
        const std::function<void(const api::StreamEvent&)>&, const std::atomic<bool>* = nullptr) override {
        return {};
    }
private:
    std::vector<std::string>& events_;
};

class ToolProbe : public tools::Tool {
public:
    ToolProbe(ScriptedMcpEndpoint& endpoint, std::vector<std::string>& events, bool& alive)
        : endpoint_(endpoint), events_(events), alive_(alive) {}
    ~ToolProbe() override {
        alive_ = endpoint_.IsAlive();
        events_.push_back("tool");
    }
    std::string name() const override { return "probe"; }
    std::string description() const override { return "fixture"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    Result execute(const nlohmann::json&) override { return {}; }
private:
    ScriptedMcpEndpoint& endpoint_;
    std::vector<std::string>& events_;
    bool& alive_;
};

assembly::SessionMcpSpec Server(std::string name, bool required = true) {
    return {{std::move(name), "explicit-command", {"explicit-arg"}, {{"ONLY_THIS", "fixture"}},
        platform::EnvMode::Replace, tools::PathToUtf8(std::filesystem::temp_directory_path())},
        {1000, 1000, nullptr}, required};
}

assembly::SessionResourcesRequest Request(std::vector<std::string>& events) {
    assembly::SessionResourcesRequest request;
    request.backend_factory = [&] { return std::make_unique<BackendProbe>(events); };
    request.registry_factory = [](std::span<const assembly::McpServerRuntime>) -> assembly::SessionRegistryResult {
        return std::make_unique<tools::ToolRegistry>();
    };
    return request;
}

assembly::SessionRegistryResult EchoRegistry(std::span<const assembly::McpServerRuntime> servers) {
    auto registry = std::make_unique<tools::ToolRegistry>();
    for (const auto& server : servers) {
        for (const auto& tool : server.tools) {
            if (tool.name == "echo") {
                registry->Register(std::make_unique<mcp::McpTool>(*server.client, server.name, tool));
            }
        }
    }
    return registry;
}
}  // namespace

TEST_CASE("session resources: validate all launch inputs before any factory or process") {
    std::vector<std::string> events;
    auto request = Request(events);
    request.mcp_servers = {Server("first"), Server("second", false)};
    SUBCASE("duplicate name") { request.mcp_servers[1].launch.name = "first"; }
    SUBCASE("missing required command") {
        request.mcp_servers[1].required = true;
        request.mcp_servers[1].launch.command.clear();
    }
    SUBCASE("missing cwd") { request.mcp_servers[1].launch.cwd_utf8.clear(); }
    SUBCASE("relative cwd") { request.mcp_servers[1].launch.cwd_utf8 = "relative-dir"; }
    SUBCASE("invalid UTF-8 cwd") { request.mcp_servers[1].launch.cwd_utf8 = std::string(1, '\xff'); }
    int backends = 0, launches = 0, registries = 0;
    request.backend_factory = [&]() -> std::unique_ptr<api::Backend> { ++backends; return nullptr; };
    request.registry_factory = [&](auto) -> assembly::SessionRegistryResult {
        ++registries;
        return std::unique_ptr<tools::ToolRegistry>{};
    };
    request.mcp_launcher = [&](const auto&) -> std::expected<std::unique_ptr<mcp::Client>, std::string> {
        ++launches;
        return std::unexpected("must not start");
    };
    auto result = assembly::BuildSessionResources(std::move(request));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().stage == assembly::SessionResourceStage::Plan);
    CHECK(result.error().code == "assembly.mcp.invalid_spec");
    CHECK(backends == 0);
    CHECK(launches == 0);
    CHECK(registries == 0);
    CHECK(events.empty());
}

TEST_CASE("session resources: absent or null backend never starts MCP") {
    std::vector<std::string> events;
    auto request = Request(events);
    request.mcp_servers = {Server("first")};
    SUBCASE("missing backend factory") { request.backend_factory = {}; }
    SUBCASE("null backend") { request.backend_factory = [] { return std::unique_ptr<api::Backend>{}; }; }
    SUBCASE("missing registry factory") { request.registry_factory = {}; }
    int launches = 0;
    request.mcp_launcher = [&](const auto&) -> std::expected<std::unique_ptr<mcp::Client>, std::string> {
        ++launches;
        return std::unexpected("must not start");
    };
    auto result = assembly::BuildSessionResources(std::move(request));
    CHECK_FALSE(result.has_value());
    CHECK(launches == 0);
    CHECK(events.empty());
}

TEST_CASE("session resources: required failure rolls back previous clients and backend") {
    for (const std::string stage : {"start", "initialize", "tools/list"}) {
        CAPTURE(stage);
        std::vector<std::string> events;
        ScriptedMcpEndpoint good, bad;
        good.tag = "good";
        bad.tag = "bad";
        good.destruction_events = bad.destruction_events = &events;
        bad.fail_method = stage;
        auto request = Request(events);
        request.mcp_servers = {Server("good"), Server("bad")};
        request.mcp_launcher = [&](const auto& launch)
            -> std::expected<std::unique_ptr<mcp::Client>, std::string> {
            if (launch.name == "bad" && stage == "start") return std::unexpected("start rejected");
            return (launch.name == "good" ? good.Launcher() : bad.Launcher())(launch);
        };
        int registries = 0;
        request.registry_factory = [&](auto) -> assembly::SessionRegistryResult {
            ++registries;
            return std::unique_ptr<tools::ToolRegistry>{};
        };
        auto result = assembly::BuildSessionResources(std::move(request));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().stage == assembly::SessionResourceStage::Mcp);
        CHECK(result.error().component == "bad");
        CHECK(result.error().mcp_stage == (stage == "start" ? assembly::McpStartupStage::Start :
            stage == "initialize" ? assembly::McpStartupStage::Initialize : assembly::McpStartupStage::Discover));
        CHECK(registries == 0);
        CHECK(good.shutdown_count == 1);
        CHECK(bad.shutdown_count == (stage == "start" ? 0 : 1));
        CHECK(events == (stage == "start" ? std::vector<std::string>{"good", "backend"}
                                          : std::vector<std::string>{"bad", "good", "backend"}));
    }
}

TEST_CASE("session resources: optional failure is reported and only successful clients reach registry") {
    std::vector<std::string> events;
    ScriptedMcpEndpoint good, bad;
    bad.fail_method = "tools/list";
    auto request = Request(events);
    request.mcp_servers = {Server("bad", false), Server("good")};
    bool empty_command = false;
    SUBCASE("optional discovery failure") {}
    SUBCASE("empty optional command is rejected before launch and reported as degradation") {
        empty_command = true;
        request.mcp_servers[0].launch.command.clear();
    }
    const auto expected_launch = request.mcp_servers[1].launch;
    request.mcp_launcher = [&](const auto& launch) {
        CHECK(launch.cwd_utf8 == expected_launch.cwd_utf8);
        CHECK(launch.command == expected_launch.command);
        CHECK(launch.args == expected_launch.args);
        CHECK(launch.env == expected_launch.env);
        CHECK(launch.env_mode == platform::EnvMode::Replace);
        return (launch.name == "good" ? good.Launcher() : bad.Launcher())(launch);
    };
    request.registry_factory = [](auto servers) {
        REQUIRE(servers.size() == 1);
        CHECK(servers[0].name == "good");
        return EchoRegistry(servers);
    };
    auto result = assembly::BuildSessionResources(std::move(request));
    REQUIRE(result.has_value());
    auto owner = std::move(*result);
    REQUIRE(owner->degraded().size() == 1);
    CHECK(owner->degraded()[0].component == "bad");
    CHECK(owner->degraded()[0].mcp_stage == (empty_command ? assembly::McpStartupStage::Start
                                                         : assembly::McpStartupStage::Discover));
    CHECK(bad.shutdown_count == (empty_command ? 0 : 1));
    CHECK(bad.methods.empty() == empty_command);
    CHECK(good.shutdown_count == 0);
    REQUIRE(owner->registry().All().size() == 1);
    auto* tool = owner->registry().Find("mcp__good__echo");
    REQUIRE(tool != nullptr);
    CHECK_FALSE(tool->execute(nlohmann::json::object()).is_error);
    CHECK(owner->registry().Find("mcp__good__zulu") == nullptr);
    owner.reset();
    CHECK(good.shutdown_count == 1);
    CHECK(events == std::vector<std::string>{"backend"});
}

TEST_CASE("session resources: registry rejection and exceptions destroy tools before clients") {
    for (const std::string mode : {"reject", "null", "throw", "success"}) {
        CAPTURE(mode);
        std::vector<std::string> events;
        ScriptedMcpEndpoint endpoint;
        endpoint.destruction_events = &events;
        bool alive_at_tool_destruction = false;
        auto request = Request(events);
        request.mcp_servers = {Server("fixture")};
        request.mcp_launcher = endpoint.Launcher();
        request.registry_factory = [&](auto) -> assembly::SessionRegistryResult {
            auto registry = std::make_unique<tools::ToolRegistry>();
            registry->Register(std::make_unique<ToolProbe>(endpoint, events, alive_at_tool_destruction));
            if (mode == "throw") throw std::runtime_error("fixture registry exception");
            if (mode == "null") return std::unique_ptr<tools::ToolRegistry>{};
            if (mode == "reject") {
                return std::unexpected(assembly::SessionResourceFailure{assembly::SessionResourceStage::Registry,
                    "host.tool_missing", "required tool unavailable", {}, {}});
            }
            return registry;
        };
        if (mode == "throw") {
            CHECK_THROWS_WITH(assembly::BuildSessionResources(std::move(request)), "fixture registry exception");
        } else {
            auto result = assembly::BuildSessionResources(std::move(request));
            if (mode == "success") {
                REQUIRE(result.has_value());
                CHECK(events.empty());
                CHECK(endpoint.IsAlive());
                result->reset();
            } else {
                REQUIRE_FALSE(result.has_value());
                CHECK(result.error().stage == assembly::SessionResourceStage::Registry);
                CHECK(result.error().code == (mode == "reject" ? "host.tool_missing" : "assembly.registry_unavailable"));
            }
        }
        CHECK(alive_at_tool_destruction);
        CHECK(events == std::vector<std::string>{"tool", "client", "backend"});
        CHECK(endpoint.shutdown_count == 1);
    }
}

TEST_CASE("session resources: closing one session leaves the other backend registry and client usable") {
    std::vector<std::string> first_events, second_events;
    ScriptedMcpEndpoint first_endpoint, second_endpoint;
    auto first = Request(first_events);
    auto second = Request(second_events);
    first.mcp_servers = second.mcp_servers = {Server("same-name")};
    first.mcp_launcher = first_endpoint.Launcher();
    second.mcp_launcher = second_endpoint.Launcher();
    first.registry_factory = second.registry_factory = EchoRegistry;
    auto first_result = assembly::BuildSessionResources(std::move(first));
    auto second_result = assembly::BuildSessionResources(std::move(second));
    REQUIRE(first_result.has_value());
    REQUIRE(second_result.has_value());
    CHECK(&(*first_result)->backend() != &(*second_result)->backend());
    CHECK(&(*first_result)->registry() != &(*second_result)->registry());
    CHECK((*first_result)->mcp_servers()[0].client.get() != (*second_result)->mcp_servers()[0].client.get());
    first_result->reset();
    CHECK(first_endpoint.shutdown_count == 1);
    CHECK(second_endpoint.shutdown_count == 0);
    auto* surviving_tool = (*second_result)->registry().Find("mcp__same-name__echo");
    REQUIRE(surviving_tool != nullptr);
    CHECK(surviving_tool->execute(nlohmann::json::object()).content == "fixture ok");
    CHECK((*second_result)->backend().send_stream({}, [](const auto&) {}).has_value());
    second_result->reset();
    CHECK(second_endpoint.shutdown_count == 1);
    CHECK(first_events == std::vector<std::string>{"backend"});
    CHECK(second_events == std::vector<std::string>{"backend"});
}
