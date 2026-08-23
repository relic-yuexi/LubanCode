// M8:mcpServers 配置解析。跟 hooks 走的是同一个套路(config-file-only,
// 没有环境变量、没有内置默认值这两级)——ParseMcpServersConfig 是纯函数,
// 不碰 IO,喂各种 JSON 形状进去,只查解析结果/错误信息。

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "config/config.hpp"

using namespace lubancode;

TEST_CASE("ParseMcpServersConfig: 完整的一份,command/args/env 都写了") {
    const auto json = nlohmann::json::parse(R"({
        "myserver": {
            "command": "python",
            "args": ["server.py", "--flag"],
            "env": {"K": "V", "K2": "V2"}
        }
    })");

    const auto result = config::ParseMcpServersConfig(json, "test.json");
    REQUIRE(result.has_value());
    REQUIRE(result->count("myserver") == 1);
    const auto& server = result->at("myserver");
    CHECK(server.command == "python");
    REQUIRE(server.args.size() == 2);
    CHECK(server.args[0] == "server.py");
    CHECK(server.args[1] == "--flag");
    REQUIRE(server.env.size() == 2);
    CHECK(server.env[0].first == "K");
    CHECK(server.env[0].second == "V");
    CHECK(server.env[1].first == "K2");
    CHECK(server.env[1].second == "V2");
}

TEST_CASE("ParseMcpServersConfig: 只写 command,args/env 缺省为空") {
    const auto json = nlohmann::json::parse(R"({"bare": {"command": "node"}})");
    const auto result = config::ParseMcpServersConfig(json, "test.json");
    REQUIRE(result.has_value());
    REQUIRE(result->count("bare") == 1);
    CHECK(result->at("bare").command == "node");
    CHECK(result->at("bare").args.empty());
    CHECK(result->at("bare").env.empty());
}

TEST_CASE("ParseMcpServersConfig: 空 object,没有任何服务器") {
    const auto json = nlohmann::json::object();
    const auto result = config::ParseMcpServersConfig(json, "test.json");
    REQUIRE(result.has_value());
    CHECK(result->empty());
}

TEST_CASE("ParseMcpServersConfig: 多个服务器各自独立解析") {
    const auto json = nlohmann::json::parse(R"({
        "a": {"command": "python"},
        "b": {"command": "node", "args": ["x.js"]}
    })");
    const auto result = config::ParseMcpServersConfig(json, "test.json");
    REQUIRE(result.has_value());
    CHECK(result->size() == 2);
    CHECK(result->at("a").command == "python");
    CHECK(result->at("b").command == "node");
}

TEST_CASE("ParseMcpServersConfig: 顶层不是 object 报错,错误信息带路径") {
    const auto json = nlohmann::json::parse(R"(["oops"])");
    const auto result = config::ParseMcpServersConfig(json, "test.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("test.json") != std::string::npos);
}

TEST_CASE("ParseMcpServersConfig: 缺 command 字段报错") {
    const auto json = nlohmann::json::parse(R"({"myserver": {"args": ["x"]}})");
    const auto result = config::ParseMcpServersConfig(json, "test.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("myserver") != std::string::npos);
    CHECK(result.error().find("command") != std::string::npos);
}

TEST_CASE("ParseMcpServersConfig: command 不是字符串报错") {
    const auto json = nlohmann::json::parse(R"({"myserver": {"command": 123}})");
    const auto result = config::ParseMcpServersConfig(json, "test.json");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ParseMcpServersConfig: 服务器的值不是 object 报错") {
    const auto json = nlohmann::json::parse(R"({"myserver": "not-an-object"})");
    const auto result = config::ParseMcpServersConfig(json, "test.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("myserver") != std::string::npos);
}

TEST_CASE("ParseMcpServersConfig: args 不是数组报错") {
    const auto json = nlohmann::json::parse(R"({"myserver": {"command": "python", "args": "not-array"}})");
    const auto result = config::ParseMcpServersConfig(json, "test.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("args") != std::string::npos);
}

TEST_CASE("ParseMcpServersConfig: args 数组元素不是字符串报错") {
    const auto json = nlohmann::json::parse(R"({"myserver": {"command": "python", "args": [1, 2]}})");
    const auto result = config::ParseMcpServersConfig(json, "test.json");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ParseMcpServersConfig: env 不是 object 报错") {
    const auto json = nlohmann::json::parse(R"({"myserver": {"command": "python", "env": ["not", "object"]}})");
    const auto result = config::ParseMcpServersConfig(json, "test.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("env") != std::string::npos);
}

TEST_CASE("ParseMcpServersConfig: env 的值不是字符串报错") {
    const auto json = nlohmann::json::parse(R"({"myserver": {"command": "python", "env": {"K": 1}}})");
    const auto result = config::ParseMcpServersConfig(json, "test.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("env") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ParseFileConfigJson / MergeConfig:mcpServers 只从配置文件来,没有环境
// 变量、没有内置默认值这两级(跟 hooks 一样),配置文件没写这字段就是空 map。
// ---------------------------------------------------------------------------

TEST_CASE("ParseFileConfigJson: 顶层带 mcpServers 段,解析进 FileConfig") {
    const std::string text = R"({
        "mcpServers": {
            "test": {"command": "python", "args": ["mcp_test_server.py"]}
        }
    })";
    const auto result = config::ParseFileConfigJson(text, "test.json");
    REQUIRE(result.has_value());
    REQUIRE(result->mcp_servers.has_value());
    CHECK(result->mcp_servers->count("test") == 1);
    CHECK(result->mcp_servers->at("test").command == "python");
}

TEST_CASE("ParseFileConfigJson: 没写 mcpServers 段,FileConfig::mcp_servers 是 nullopt") {
    const std::string text = R"({"model": "foo"})";
    const auto result = config::ParseFileConfigJson(text, "test.json");
    REQUIRE(result.has_value());
    CHECK_FALSE(result->mcp_servers.has_value());
}

TEST_CASE("MergeConfig: 配置文件写了 mcpServers,合并结果里带过去") {
    config::FileConfig file;
    file.source_path = "/tmp/.lubancode.json";
    std::map<std::string, config::McpServerConfig> servers;
    config::McpServerConfig server;
    server.command = "python";
    server.args = {"server.py"};
    servers.emplace("test", server);
    file.mcp_servers = servers;

    const auto result = config::MergeConfig(config::LubancodeEnvValues{}, file, config::GenericEnvValues{});
    REQUIRE(result.has_value());
    REQUIRE(result->config.mcp_servers.count("test") == 1);
    CHECK(result->config.mcp_servers.at("test").command == "python");
}

TEST_CASE("MergeConfig: 配置文件没写 mcpServers,合并结果里是空 map") {
    config::FileConfig file;
    file.source_path = "/tmp/.lubancode.json";

    const auto result = config::MergeConfig(config::LubancodeEnvValues{}, file, config::GenericEnvValues{});
    REQUIRE(result.has_value());
    CHECK(result->config.mcp_servers.empty());
}

TEST_CASE("MergeConfig: 没有配置文件(std::nullopt),mcpServers 是空 map") {
    const auto result = config::MergeConfig(config::LubancodeEnvValues{}, std::nullopt, config::GenericEnvValues{});
    REQUIRE(result.has_value());
    CHECK(result->config.mcp_servers.empty());
}
