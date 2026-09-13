// 会话级运行材料装配的单测(工业化多协议接入单 P1,G01/G02)。
//
// 钉的规矩(冻结合同 §7 最小形状 + 单子 P1):
//   - 注入路:backend/registry 工厂各调一次、零 MCP、档案显式;
//   - 无部署档 = 显式零工具默认档(空表,不照搬终端全部工具);
//   - 零工具档(zero-tools)零启动:config 配了 MCP 也不碰;
//   - 先解析再启动:必需服务缺上层配置即明拒,零进程启动;
//   - 必需服务起服失败(command 不存在)整场明拒,不出半成品;
//   - 可选服务起服失败降级记账,不冒充已挂;
//   - 步数闸与 system_prompt 原样进档案(装配不猜)。
//
// 真进程链(必需服务握手后工具复验)在 integration 册
// test_app_server_profile_smoke.cpp——python 夹具配真 exe 走。
#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "api/backend.hpp"
#include "app_server/harness_profile.hpp"
#include "app_server/session_assembly.hpp"
#include "config/config.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"

using namespace lubancode;
using namespace lubancode::app_server;

namespace {

// 假 backend(test_app_server_service.cpp 同款思路):装配只要求工厂交
// 非空件,本册不发请求。
class StubBackend : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(const api::Request&,
                                                const std::function<void(const api::StreamEvent&)>&,
                                                const std::atomic<bool>* = nullptr) override {
        return {};
    }
};

std::unique_ptr<api::Backend> MakeFakeBackend() {
    return std::make_unique<StubBackend>();
}

SessionAssemblyRequest BaseRequest() {
    SessionAssemblyRequest request;
    request.backend_factory = &MakeFakeBackend;
    request.system_prompt = "test prompt";
    request.max_steps_per_turn = 7;
    return request;
}

// 一枚 only 档的 harness(直接构造——JSON 解析路另有
// test_harness_profile.cpp 把关)。
HarnessProfile MakeOnlyProfile(std::vector<std::string> allow, std::vector<std::string> servers) {
    HarnessProfile profile;
    profile.name = "only";
    profile.tools.mode = HarnessToolPolicy::Mode::Only;
    profile.tools.allow = std::move(allow);
    profile.mcp_servers = std::move(servers);
    profile.features_enabled.insert("mcp");
    return profile;
}

config::Config MakeConfigWithServer(const std::string& name, const std::string& command) {
    config::Config config;
    config::McpServerConfig server;
    server.command = command;
    config.mcp_servers[name] = server;
    return config;
}

}  // namespace

TEST_CASE("注入路:backend/registry 工厂各交一件,零 MCP,档案原样") {
    SessionAssemblyRequest request = BaseRequest();
    int registry_calls = 0;
    request.registry_factory = [&registry_calls]() {
        ++registry_calls;
        auto registry = std::make_unique<tools::ToolRegistry>();
        registry->Register(std::make_unique<tools::ReadFileTool>());
        return registry;
    };
    const auto result = AssembleSession(std::move(request));
    REQUIRE(result.assembly != nullptr);
    CHECK(result.error.empty());
    CHECK(result.assembly->backend != nullptr);
    REQUIRE(result.assembly->registry != nullptr);
    CHECK(result.assembly->registry->Find("read_file") != nullptr);
    CHECK(result.assembly->mcp_servers.empty());
    CHECK(result.assembly->degraded_components.empty());
    CHECK(result.assembly->agent_profile.system_prompt == "test prompt");
    CHECK(result.assembly->agent_profile.runtime.max_steps_per_turn == 7);
    CHECK(registry_calls == 1);
}

TEST_CASE("无部署档 = 显式零工具默认档:空表,不照搬终端工具") {
    SessionAssemblyRequest request = BaseRequest();
    const auto result = AssembleSession(std::move(request));
    REQUIRE(result.assembly != nullptr);
    REQUIRE(result.assembly->registry != nullptr);
    CHECK(result.assembly->registry->All().empty());  // 没有 run_command/write_file 一类
    CHECK(result.assembly->mcp_servers.empty());
}

TEST_CASE("零工具档零启动:config 配了 MCP 也不碰") {
    SessionAssemblyRequest request = BaseRequest();
    config::Config config = MakeConfigWithServer("tools-approved", "no-such-command");
    HarnessProfile harness;
    harness.name = "zero";
    harness.tools.mode = HarnessToolPolicy::Mode::None;
    request.config = &config;
    request.harness = &harness;
    const auto result = AssembleSession(std::move(request));
    REQUIRE(result.assembly != nullptr);
    CHECK(result.assembly->mcp_servers.empty());  // 合同 §2.3:不启动 MCP 握手
    REQUIRE(result.assembly->registry != nullptr);
    CHECK(result.assembly->registry->All().empty());
}

TEST_CASE("先解析再启动:必需服务缺上层配置即明拒") {
    SessionAssemblyRequest request = BaseRequest();
    config::Config config;  // 空 config:mcp_servers 一枚没有
    HarnessProfile harness = MakeOnlyProfile({"mcp:tools-approved:echo"}, {"tools-approved"});
    request.config = &config;
    request.harness = &harness;
    const auto result = AssembleSession(std::move(request));
    CHECK(result.assembly == nullptr);  // 明拒,不出半成品
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("必需服务起服失败整场明拒(command 不存在)") {
    SessionAssemblyRequest request = BaseRequest();
    config::Config config = MakeConfigWithServer("tools-approved", "definitely-no-such-command-xyz");
    HarnessProfile harness = MakeOnlyProfile({"mcp:tools-approved:echo"}, {"tools-approved"});
    request.config = &config;
    request.harness = &harness;
    const auto result = AssembleSession(std::move(request));
    CHECK(result.assembly == nullptr);
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("backend 工厂缺失或交空件:装配明拒") {
    SUBCASE("工厂缺失") {
        SessionAssemblyRequest request;
        request.system_prompt = "x";
        const auto result = AssembleSession(std::move(request));
        CHECK(result.assembly == nullptr);
        CHECK_FALSE(result.error.empty());
    }
    SUBCASE("交空件") {
        SessionAssemblyRequest request;
        request.backend_factory = []() { return nullptr; };
        const auto result = AssembleSession(std::move(request));
        CHECK(result.assembly == nullptr);
        CHECK_FALSE(result.error.empty());
    }
    SUBCASE("注入 registry 工厂交空件") {
        SessionAssemblyRequest request = BaseRequest();
        request.registry_factory = []() { return nullptr; };
        const auto result = AssembleSession(std::move(request));
        CHECK(result.assembly == nullptr);
        CHECK_FALSE(result.error.empty());
    }
}
