// config 的 lsp 段解析(ParseLspServersConfig,纯函数)+ 四级合并里 lsp
// 的待遇(只从配置文件来,没配 = 空 map)。

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "config/config.hpp"

using namespace lubancode;

TEST_CASE("ParseLspServersConfig: 完整配置(command/args/extensions/idle_minutes)解析正确") {
    const nlohmann::json j = nlohmann::json::parse(R"({
        "cpp": {
            "command": "clangd",
            "args": ["--background-index=false", "--malloc-trim"],
            "extensions": [".cpp", ".hpp", ".cc", ".h"],
            "idle_minutes": 5
        },
        "python": {
            "command": "pyright-langserver",
            "args": ["--stdio"],
            "extensions": [".py"]
        }
    })");
    const auto result = config::ParseLspServersConfig(j, "test.json");
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 2);

    const auto& cpp = result->at("cpp");
    CHECK(cpp.command == "clangd");
    REQUIRE(cpp.args.size() == 2);
    CHECK(cpp.args[0] == "--background-index=false");
    REQUIRE(cpp.extensions.size() == 4);
    CHECK(cpp.extensions[0] == ".cpp");
    CHECK(cpp.idle_minutes == 5);

    const auto& python = result->at("python");
    CHECK(python.command == "pyright-langserver");
    CHECK(python.idle_minutes == 10);  // 没写就是默认 10
}

TEST_CASE("ParseLspServersConfig: args 可省,缺省是空数组") {
    const nlohmann::json j = nlohmann::json::parse(R"({"go": {"command": "gopls", "extensions": [".go"]}})");
    const auto result = config::ParseLspServersConfig(j, "test.json");
    REQUIRE(result.has_value());
    CHECK(result->at("go").args.empty());
}

TEST_CASE("ParseLspServersConfig: 缺 command 报错,错误信息带语言名") {
    const nlohmann::json j = nlohmann::json::parse(R"({"cpp": {"extensions": [".cpp"]}})");
    const auto result = config::ParseLspServersConfig(j, "test.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("lsp.cpp") != std::string::npos);
    CHECK(result.error().find("command") != std::string::npos);
}

TEST_CASE("ParseLspServersConfig: extensions 缺失/空数组都报错") {
    const nlohmann::json missing = nlohmann::json::parse(R"({"cpp": {"command": "clangd"}})");
    const auto r1 = config::ParseLspServersConfig(missing, "test.json");
    REQUIRE_FALSE(r1.has_value());
    CHECK(r1.error().find("extensions") != std::string::npos);

    const nlohmann::json empty = nlohmann::json::parse(R"({"cpp": {"command": "clangd", "extensions": []}})");
    const auto r2 = config::ParseLspServersConfig(empty, "test.json");
    REQUIRE_FALSE(r2.has_value());
    CHECK(r2.error().find("extensions") != std::string::npos);
}

TEST_CASE("ParseLspServersConfig: idle_minutes 不是正整数报错") {
    const nlohmann::json j =
        nlohmann::json::parse(R"({"cpp": {"command": "clangd", "extensions": [".cpp"], "idle_minutes": 0}})");
    const auto result = config::ParseLspServersConfig(j, "test.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("idle_minutes") != std::string::npos);
}

TEST_CASE("ParseLspServersConfig: 顶层不是 object 报错") {
    const auto result = config::ParseLspServersConfig(nlohmann::json::array(), "test.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("lsp") != std::string::npos);
}

TEST_CASE("ParseFileConfigJson + MergeConfig: 配置文件里的 lsp 段一路进到最终 Config") {
    const std::string json_text = R"({
        "lsp": {"cpp": {"command": "clangd", "extensions": [".cpp", ".hpp"]}}
    })";
    const auto file_config = config::ParseFileConfigJson(json_text, "test.json");
    REQUIRE(file_config.has_value());
    REQUIRE(file_config->lsp_servers.has_value());

    const auto merged =
        config::MergeConfig(config::LubancodeEnvValues{}, *file_config);
    REQUIRE(merged.has_value());
    REQUIRE(merged->config.lsp_servers.size() == 1);
    CHECK(merged->config.lsp_servers.at("cpp").command == "clangd");
}

TEST_CASE("MergeConfig: 配置文件没写 lsp 段,最终就是空 map(不启用)") {
    const auto file_config = config::ParseFileConfigJson("{}", "test.json");
    REQUIRE(file_config.has_value());
    const auto merged =
        config::MergeConfig(config::LubancodeEnvValues{}, *file_config);
    REQUIRE(merged.has_value());
    CHECK(merged->config.lsp_servers.empty());
}

TEST_CASE("ParseFileConfigJson: lsp 段里的错误(缺 extensions)带着文件路径往上冒") {
    const std::string json_text = R"({"lsp": {"cpp": {"command": "clangd"}}})";
    const auto file_config = config::ParseFileConfigJson(json_text, "my_config.json");
    REQUIRE_FALSE(file_config.has_value());
    CHECK(file_config.error().find("my_config.json") != std::string::npos);
    CHECK(file_config.error().find("extensions") != std::string::npos);
}
