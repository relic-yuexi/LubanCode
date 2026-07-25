// 初次配置向导:纯逻辑测试,脚本化输入序列 + 假的 print 收集器 + 假的
// fetch_models,不碰真实 stdin/stdout,也不真发网络请求。

#include <doctest/doctest.h>

#include "cli/setup_wizard.hpp"

using namespace lubancode;

namespace {

// 按顺序喂一串"用户输入",read_line() 依次吐出来,吐完了返回 EOF
// (std::nullopt)。print() 把每一行攒进 printed,方便断言里查关键字。
struct ScriptedIO {
    std::vector<std::string> inputs;
    std::size_t next = 0;
    std::vector<std::string> printed;
    cli::WizardFetchModelsFn fetch = [](config::Wire, const std::string&,
                                         const std::string&) -> std::expected<std::vector<api::ModelInfo>, api::Error> {
        return std::vector<api::ModelInfo>{};  // 默认不会被用到(测试没喂空 model 的话)
    };

    cli::WizardIO Build() {
        cli::WizardIO io;
        io.print = [this](const std::string& line) { printed.push_back(line); };
        io.read_line = [this]() -> std::optional<std::string> {
            if (next >= inputs.size()) {
                return std::nullopt;
            }
            return inputs[next++];
        };
        io.fetch_models = fetch;
        io.home_config_display_path = "/fake/home/.lubancode.json";
        return io;
    }

    bool AnyPrintedContains(const std::string& needle) const {
        for (const auto& line : printed) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

}  // namespace

TEST_CASE("RunSetupWizard: 全部手填,直接输入模型名,默认保存") {
    ScriptedIO scripted;
    scripted.inputs = {
        "",                                 // 语言:回车 -> 默认(当前语言)
        "1",                                // wire = anthropic
        "https://api.example.com/anthropic/",  // base_url,带尾部斜杠
        "sk-test-key-1234567890",           // api_key
        "MyModel",                          // model,直接输入,不走列表
        "",                                  // 保存? 直接回车 -> 默认 Y
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunSetupWizard(io);

    REQUIRE(outcome.has_value());
    CHECK(outcome->config.wire == config::Wire::Anthropic);
    CHECK(outcome->config.base_url == "https://api.example.com/anthropic");  // 尾部斜杠被剥掉
    CHECK(outcome->config.auth_token == "sk-test-key-1234567890");
    CHECK(outcome->config.model == "MyModel");
    CHECK(outcome->save_requested == true);
    CHECK(scripted.AnyPrintedContains("sk-test-..."));  // 汇总展示时 api_key 打码
}

TEST_CASE("RunSetupWizard: wire 选 2 (responses),回车走 model 列表选号") {
    ScriptedIO scripted;
    scripted.fetch = [](config::Wire wire, const std::string& base_url,
                         const std::string& api_key) -> std::expected<std::vector<api::ModelInfo>, api::Error> {
        CHECK(wire == config::Wire::Responses);
        CHECK(base_url == "https://api.example.com/v1");
        CHECK(api_key == "the-key");
        return std::vector<api::ModelInfo>{
            {"model-a", "Model A"},
            {"model-b", "Model B"},
            {"model-c", "Model C"},
        };
    };
    scripted.inputs = {
        "",                               // 语言:回车 -> 默认
        "2",                              // wire = responses
        "https://api.example.com/v1",     // base_url
        "the-key",                        // api_key
        "",                                // model:回车 -> 走列表
        "2",                               // 选第 2 个
        "n",                               // 不保存
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunSetupWizard(io);

    REQUIRE(outcome.has_value());
    CHECK(outcome->config.wire == config::Wire::Responses);
    CHECK(outcome->config.model == "model-b");
    CHECK(outcome->save_requested == false);
}

TEST_CASE("RunSetupWizard: wire 选 3 是 chat_completions") {
    ScriptedIO scripted;
    scripted.inputs = {"", "3", "https://api.example.com/v1", "key", "chat-model", "n"};
    auto io = scripted.Build();
    const auto outcome = cli::RunSetupWizard(io);
    REQUIRE(outcome.has_value());
    CHECK(outcome->config.wire == config::Wire::ChatCompletions);
    CHECK(outcome->config.model == "chat-model");
}

TEST_CASE("RunSetupWizard: model 列表拉取失败,回落到手输模型名") {
    ScriptedIO scripted;
    scripted.fetch = [](config::Wire, const std::string&,
                         const std::string&) -> std::expected<std::vector<api::ModelInfo>, api::Error> {
        return std::unexpected(api::Error{api::ErrorKind::Network, "连接被拒绝", 0});
    };
    scripted.inputs = {
        "",                                // 语言:回车 -> 默认
        "1",                               // wire
        "https://api.example.com/anthropic",
        "the-key",
        "",                                 // model:回车 -> 尝试拉列表,失败
        "FallbackModel",                    // 回落手输
        "Y",                                 // 保存
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunSetupWizard(io);

    REQUIRE(outcome.has_value());
    CHECK(outcome->config.model == "FallbackModel");
    CHECK(scripted.AnyPrintedContains("连接被拒绝"));
}

TEST_CASE("RunSetupWizard: model 列表是空的,同样回落到手输") {
    ScriptedIO scripted;
    scripted.fetch = [](config::Wire, const std::string&,
                         const std::string&) -> std::expected<std::vector<api::ModelInfo>, api::Error> {
        return std::vector<api::ModelInfo>{};
    };
    scripted.inputs = {
        "", "1", "https://api.example.com/anthropic", "the-key", "", "TypedModel", "Y",
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunSetupWizard(io);

    REQUIRE(outcome.has_value());
    CHECK(outcome->config.model == "TypedModel");
}

TEST_CASE("RunSetupWizard: model 编号选择留空走默认第一个") {
    ScriptedIO scripted;
    scripted.fetch = [](config::Wire, const std::string&,
                         const std::string&) -> std::expected<std::vector<api::ModelInfo>, api::Error> {
        return std::vector<api::ModelInfo>{{"first-model", ""}, {"second-model", "Second"}};
    };
    scripted.inputs = {
        "",  // 语言:回车 -> 默认
        "",  // wire:回车 -> 默认 1 (anthropic)
        "https://api.example.com/anthropic",
        "the-key",
        "",  // model:回车 -> 走列表
        "",  // 编号:回车 -> 默认第 1 个
        "",  // 保存:回车 -> 默认 Y
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunSetupWizard(io);

    REQUIRE(outcome.has_value());
    CHECK(outcome->config.wire == config::Wire::Anthropic);
    CHECK(outcome->config.model == "first-model");
    CHECK(outcome->save_requested == true);
}

TEST_CASE("RunSetupWizard: base_url/api_key 空输入会被拒绝,重新问") {
    ScriptedIO scripted;
    scripted.inputs = {
        "",                                  // 语言:回车 -> 默认
        "1",
        "",                                  // base_url 空,拒绝
        "https://api.example.com/anthropic",  // 重新输入,通过
        "",                                   // api_key 空,拒绝
        "the-key",                            // 重新输入,通过
        "SomeModel",
        "n",
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunSetupWizard(io);

    REQUIRE(outcome.has_value());
    CHECK(outcome->config.base_url == "https://api.example.com/anthropic");
    CHECK(outcome->config.auth_token == "the-key");
}

TEST_CASE("RunSetupWizard: 中途 EOF,返回 std::nullopt") {
    ScriptedIO scripted;
    scripted.inputs = {
        "",  // 语言:回车 -> 默认
        "1",
        "https://api.example.com/anthropic",
        // api_key 这一步之后没有更多输入了 -> EOF
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunSetupWizard(io);
    CHECK_FALSE(outcome.has_value());
}

TEST_CASE("RunSetupWizard: wire 编号不认得时重问,直到给出合法值") {
    ScriptedIO scripted;
    scripted.inputs = {
        "",   // 语言:回车 -> 默认
        "9",  // 不合法
        "abc", // 不合法
        "2",  // responses
        "https://api.example.com/v1",
        "the-key",
        "TypedModel",
        "n",
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunSetupWizard(io);

    REQUIRE(outcome.has_value());
    CHECK(outcome->config.wire == config::Wire::Responses);
}
