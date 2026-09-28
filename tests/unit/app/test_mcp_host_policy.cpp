#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "app/tool_runtime.hpp"
#include "app_server/session_assembly.hpp"
#include "runtime/assembly/mcp.hpp"
#include "scripted_mcp_endpoint.hpp"
#include "tools/path_utils.hpp"

using namespace lubancode;

namespace {
namespace assembly = runtime::assembly;
using test_support::ScriptedMcpEndpoint;

static_assert(std::is_same_v<app::McpServerRuntime, assembly::McpServerRuntime>);
static_assert(std::is_same_v<app_server::HeadlessMcpRuntime, assembly::McpServerRuntime>);

class StubBackend : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(const api::Request&,
        const std::function<void(const api::StreamEvent&)>&, const std::atomic<bool>* = nullptr) override {
        return {};
    }
};

app_server::SessionAssemblyRequest Request(const config::Config& config,
                                          const app_server::HarnessProfile* harness) {
    app_server::SessionAssemblyRequest request;
    request.cwd_utf8 = tools::PathToUtf8(std::filesystem::temp_directory_path());
    request.config = &config;
    request.harness = harness;
    request.backend_factory = [] { return std::make_unique<StubBackend>(); };
    request.system_prompt = "fixture prompt";
    return request;
}

app_server::HarnessProfile Profile(std::vector<std::string> servers, std::vector<std::string> allow) {
    app_server::HarnessProfile profile;
    profile.name = "fixture";
    profile.features_enabled.insert("mcp");
    profile.mcp_servers = std::move(servers);
    profile.tools.mode = app_server::HarnessToolPolicy::Mode::Only;
    profile.tools.allow = std::move(allow);
    return profile;
}

config::Config Config(std::initializer_list<std::string> names) {
    config::Config config;
    for (const auto& name : names) config.mcp_servers[name].command = "fixture-unused-command";
    return config;
}

// 不读已销毁 Client；独立 transport 留账，若宿主颠倒字段顺序，断言会失败。
class DestructionProbe : public tools::Tool {
public:
    DestructionProbe(ScriptedMcpEndpoint& endpoint, std::vector<std::string>& events, bool& was_alive)
        : endpoint_(endpoint), events_(events), was_alive_(was_alive) {}
    ~DestructionProbe() override {
        was_alive_ = endpoint_.IsAlive();
        events_.push_back("tool");
    }
    std::string name() const override { return "destruction_probe"; }
    std::string description() const override { return "fixture"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    Result execute(const nlohmann::json&) override { return {}; }
private:
    ScriptedMcpEndpoint& endpoint_;
    std::vector<std::string>& events_;
    bool& was_alive_;
};
}  // namespace

TEST_CASE("MCP host policy: CLI keeps startup failure optional") {
    auto config = Config({"missing"});
    config.mcp_servers["missing"].command = "lubancode-sdk-test-command-that-does-not-exist-53a810";
    CHECK(app::StartMcpServers(config.mcp_servers, cli::Theme{}).empty());
}

TEST_CASE("MCP host policy: headless never starts servers without an approved deployment name") {
    const auto config = Config({"configured"});
    auto profile = Profile({"unapproved"}, {"mcp:unapproved:echo"});
    int launches = 0;
    auto request = Request(config, nullptr);
    request.mcp_launcher = [&](const auto&) -> std::expected<std::unique_ptr<mcp::Client>, std::string> {
        ++launches;
        return std::unexpected("must not launch");
    };
    SUBCASE("no deployment remains zero tools") {
        auto result = app_server::AssembleSession(request);
        REQUIRE(result.assembly != nullptr);
        CHECK(result.assembly->resources->registry().All().empty());
        CHECK(result.assembly->resources->mcp_servers().empty());
    }
    SUBCASE("required but absent from upper configuration is rejected before launch") {
        request.harness = &profile;
        CHECK(app_server::AssembleSession(request).assembly == nullptr);
    }
    CHECK(launches == 0);
}

TEST_CASE("MCP host policy: optional stage failures degrade while required failures reject") {
    for (const std::string stage : {"start", "initialize", "tools/list"}) {
        for (bool required : {false, true}) {
            CAPTURE(stage);
            CAPTURE(required);
            ScriptedMcpEndpoint endpoint;
            endpoint.fail_method = stage;
            const auto config = Config({"approved"});
            auto profile = Profile({"approved"}, required ? std::vector<std::string>{"mcp:approved:echo"}
                                                          : std::vector<std::string>{});
            auto request = Request(config, &profile);
            request.mcp_launcher = [&](const auto& launch)
                -> std::expected<std::unique_ptr<mcp::Client>, std::string> {
                CHECK(launch.env_mode == platform::EnvMode::Replace);
                if (stage == "start") return std::unexpected("fixture start failure");
                return endpoint.Launcher()(launch);
            };
            auto result = app_server::AssembleSession(request);
            if (required) {
                CHECK(result.assembly == nullptr);
                CHECK(result.error.find(stage == "tools/list" ? "工具清单拉取失败" : "起服失败") != std::string::npos);
            } else {
                REQUIRE(result.assembly != nullptr);
                CHECK(result.assembly->resources->mcp_servers().empty());
                CHECK(result.assembly->resources->registry().All().empty());
                REQUIRE(result.assembly->degraded_components.size() == 1);
                CHECK(result.assembly->degraded_components.front().find("fixture") != std::string::npos);
            }
            CHECK(endpoint.shutdown_count == (stage == "start" ? 0 : 1));
        }
    }
}

TEST_CASE("MCP host policy: shared discovered tools keep CLI and headless selection separate") {
    ScriptedMcpEndpoint cli_endpoint;
    ScriptedMcpEndpoint headless_endpoint;
    auto ready = assembly::StartMcpServer({"approved", "fixture", {}, {}, platform::EnvMode::Inherit},
                                          {}, cli_endpoint.Launcher());
    REQUIRE(ready.has_value());
    std::vector<app::McpServerRuntime> owners;
    owners.push_back(std::move(*ready));
    tools::ToolRegistry cli_registry;
    app::RegisterMcpTools(owners, cli_registry);
    REQUIRE(cli_registry.All().size() == 2);
    CHECK(cli_registry.All()[0]->name() == "mcp__approved__echo");
    CHECK(cli_registry.All()[1]->name() == "mcp__approved__zulu");
    for (const auto& tool : cli_registry.All()) {
        CHECK(tool->deferred());
        CHECK(cli_registry.RegistrationOf(tool->name())->source_kind == tools::ToolSourceKind::Mcp);
    }

    const auto config = Config({"approved", "not-selected"});
    auto profile = Profile({"approved"}, {"mcp:approved:echo"});
    auto request = Request(config, &profile);
    std::vector<std::string> launched;
    request.mcp_launcher = [&](const auto& launch) {
        launched.push_back(launch.name);
        CHECK(launch.env_mode == platform::EnvMode::Replace);
        return headless_endpoint.Launcher()(launch);
    };
    auto result = app_server::AssembleSession(request);
    REQUIRE(result.assembly != nullptr);
    CHECK(launched == std::vector<std::string>{"approved"});
    REQUIRE(result.assembly->resources->registry().All().size() == 1);
    const auto* tool = result.assembly->resources->registry().Find("mcp__approved__echo");
    REQUIRE(tool != nullptr);
    CHECK_FALSE(tool->deferred());
    CHECK(result.assembly->resources->registry().Find("mcp__approved__zulu") == nullptr);
}

TEST_CASE("MCP host policy: required failure rolls back current and previously started clients") {
    std::vector<std::string> events;
    ScriptedMcpEndpoint good;
    ScriptedMcpEndpoint bad;
    good.destruction_events = &events;
    good.tag = "good";
    bad.destruction_events = &events;
    bad.tag = "bad";
    bad.fail_method = "initialize";
    const auto config = Config({"good", "bad"});
    auto profile = Profile({"good", "bad"}, {"mcp:good:echo", "mcp:bad:echo"});
    auto request = Request(config, &profile);
    request.mcp_launcher = [&](const auto& launch) {
        return (launch.name == "good" ? good.Launcher() : bad.Launcher())(launch);
    };
    CHECK(app_server::AssembleSession(request).assembly == nullptr);
    CHECK(events == std::vector<std::string>{"bad", "good"});
    CHECK(good.shutdown_count == 1);
    CHECK(bad.shutdown_count == 1);
}

TEST_CASE("MCP host policy: actual SessionAssembly destroys its registry before MCP owners") {
    std::vector<std::string> events;
    bool client_alive_when_tool_destroyed = false;
    ScriptedMcpEndpoint endpoint;
    endpoint.destruction_events = &events;
    {
        const auto config = Config({"fixture"});
        auto profile = Profile({"fixture"}, {"mcp:fixture:echo"});
        auto request = Request(config, &profile);
        request.mcp_launcher = endpoint.Launcher();
        auto assembled = app_server::AssembleSession(std::move(request));
        REQUIRE(assembled.assembly != nullptr);
        assembled.assembly->resources->registry().Register(
            std::make_unique<DestructionProbe>(endpoint, events, client_alive_when_tool_destroyed));
    }
    CHECK(client_alive_when_tool_destroyed);
    CHECK(events == std::vector<std::string>{"tool", "client"});
}
