// /provider add 向导:纯逻辑测试,脚本化输入序列 + 假的 print 收集器 + 假的
// fetch_models,不碰真实 stdin/stdout,也不真发网络请求。跟 test_setup_wizard.cpp
// 用的是同一套 ScriptedIO 手法。

#include <doctest/doctest.h>

#include "cli/provider_wizard.hpp"

using namespace lubancode;

namespace {

struct ScriptedIO {
    std::vector<std::string> inputs;
    std::size_t next = 0;
    std::vector<std::string> printed;
    cli::WizardFetchModelsFn fetch = [](config::Wire, const std::string&,
                                         const std::string&) -> std::expected<std::vector<api::ModelInfo>, api::Error> {
        return std::vector<api::ModelInfo>{};
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

TEST_CASE("RunProviderAddWizard: 全部手填,直接贴 key、手输模型、设置 effort,确认写入") {
    ScriptedIO scripted;
    scripted.inputs = {
        "sub-openai",                          // 名字
        "https://cc.moontidef.work/",           // base_url,带尾部斜杠
        "2",                                     // wire = responses
        "sk-test-key-1234567890",               // api_key 直贴
        "gpt-5.5",                               // model 手输
        "xhigh",                                 // effort
        "{\"thinking\":{\"type\":\"enabled\"},\"reasoning_effort\":\"max\"}",  // extra_body
        "",                                       // 确认:回车 -> 默认 Y
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});

    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.name == "sub-openai");
    CHECK(outcome->provider.base_url == "https://cc.moontidef.work");  // 尾部斜杠被剥掉
    CHECK(outcome->provider.wire == config::Wire::Responses);
    CHECK(outcome->provider.api_key == "sk-test-key-1234567890");
    CHECK(outcome->provider.model == "gpt-5.5");
    CHECK(outcome->provider.model_reasoning_effort == "xhigh");
    CHECK(outcome->provider.extra_body.at("reasoning_effort") == "max");
    CHECK(outcome->provider.extra_body.at("thinking").at("type") == "enabled");
    CHECK(outcome->save_requested == true);
    CHECK(scripted.AnyPrintedContains("sk-test-..."));  // 汇总展示时 api_key 打码,不落明文
    CHECK_FALSE(scripted.AnyPrintedContains("sk-test-key-1234567890"));
}

TEST_CASE("RunProviderAddWizard: 命令行给了合法名字就跳过名字这一步") {
    ScriptedIO scripted;
    scripted.inputs = {
        // 没有"名字"这一问了,第一条输入直接是 base_url
        "https://api.example.test",
        "1",              // wire = anthropic
        "the-key",        // api_key 直贴
        "MyModel",        // model
        "",                // effort:留空跳过
        "",                // extra_body:留空跳过
        "n",               // 不确认
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "myprovider", {});

    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.name == "myprovider");
    CHECK(outcome->provider.model_reasoning_effort.empty());
    CHECK(outcome->provider.extra_body.empty());
    CHECK(outcome->save_requested == false);
}

TEST_CASE("RunProviderAddWizard: 命令行给的名字重名或非法字符,回落到交互式重问") {
    const std::vector<config::ProviderConfig> existing{{.name = "dup", .base_url = "https://x.test"}};

    ScriptedIO scripted;
    scripted.inputs = {
        "new-name",                  // 重问后给一个合法且不重名的名字
        "https://api.example.test",
        "1",
        "the-key",
        "MyModel",
        "",
        "",
        "Y",
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "dup", existing);

    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.name == "new-name");
    CHECK(scripted.AnyPrintedContains("dup"));  // 提示过命令行给的名字不能用
}

TEST_CASE("RunProviderAddWizard: base_url 没有 http(s) 前缀会被拒绝,重新问") {
    ScriptedIO scripted;
    scripted.inputs = {
        "p1",
        "not-a-url",                  // 拒绝
        "ftp://example.test",         // 也拒绝(协议不对)
        "https://api.example.test",   // 通过
        "1",
        "the-key",
        "MyModel",
        "",
        "",
        "Y",
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});

    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.base_url == "https://api.example.test");
}

TEST_CASE("RunProviderAddWizard: api_key 留空改问 key_env,留空按默认 ANTHROPIC_AUTH_TOKEN") {
    ScriptedIO scripted;
    scripted.inputs = {
        "p1",
        "https://api.example.test",
        "1",
        "",   // api_key 留空
        "",   // key_env 留空 -> 默认
        "MyModel",
        "",
        "",
        "Y",
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});

    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.api_key.empty());
    CHECK(outcome->provider.key_env == "ANTHROPIC_AUTH_TOKEN");
}

TEST_CASE("RunProviderAddWizard: api_key 留空、key_env 自定义,拉模型列表时用的是环境变量里的值") {
    ScriptedIO scripted;
    scripted.fetch = [](config::Wire wire, const std::string& base_url,
                         const std::string& api_key) -> std::expected<std::vector<api::ModelInfo>, api::Error> {
        CHECK(wire == config::Wire::Responses);
        CHECK(base_url == "https://api.example.test");
        // key_env 指向的环境变量在测试环境里多半没设置,取不到值时 fetch_key
        // 应该是空字符串,而不是 key_env 这个名字本身。
        CHECK(api_key.empty());
        return std::vector<api::ModelInfo>{{"model-a", "Model A"}};
    };
    scripted.inputs = {
        "p1",
        "https://api.example.test",
        "2",                 // wire = responses
        "",                   // api_key 留空
        "SOME_UNSET_ENV_FOR_TEST",  // key_env
        "",                    // model:回车走列表
        "1",                    // 选第一个
        "",
        "",
        "Y",
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});

    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.model == "model-a");
    CHECK(outcome->provider.key_env == "SOME_UNSET_ENV_FOR_TEST");
}

TEST_CASE("RunProviderAddWizard: effort 留空 = 不设置,汇总展示未设置") {
    ScriptedIO scripted;
    scripted.inputs = {
        "p1", "https://api.example.test", "1", "the-key", "MyModel", "", "", "Y",
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});

    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.model_reasoning_effort.empty());
}

TEST_CASE("RunProviderAddWizard: 最后一问答 n,save_requested 为 false") {
    ScriptedIO scripted;
    scripted.inputs = {
        "p1", "https://api.example.test", "1", "the-key", "MyModel", "high", "", "n",
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});

    REQUIRE(outcome.has_value());
    CHECK(outcome->save_requested == false);
}

TEST_CASE("RunProviderAddWizard: extra_body 不合法 JSON 会重问,合法 object 才通过") {
    ScriptedIO scripted;
    scripted.inputs = {
        "p1", "https://api.example.test", "1", "the-key", "MyModel", "",
        "{not json",           // 解析失败,重问
        "[1,2,3]",             // 解析成功但不是 object,重问
        "{\"a\":1}",           // 合法 object,通过
        "Y",
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});

    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.extra_body.at("a") == 1);
    CHECK(scripted.AnyPrintedContains("不是合法 JSON"));       // 解析失败那次提示过
    CHECK(scripted.AnyPrintedContains("JSON object"));         // 不是 object 那次提示过
}

TEST_CASE("RunProviderAddWizard: extra_body 中途 EOF 返回 std::nullopt") {
    ScriptedIO scripted;
    scripted.inputs = {
        "p1", "https://api.example.test", "1", "the-key", "MyModel", "high",
        // extra_body 这一步之后没有更多输入了 -> EOF
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    CHECK_FALSE(outcome.has_value());
}

TEST_CASE("RunProviderAddWizard: 中途 EOF 返回 std::nullopt") {
    ScriptedIO scripted;
    scripted.inputs = {
        "p1", "https://api.example.test",
        // wire 这一步之后没有更多输入了 -> EOF
    };
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    CHECK_FALSE(outcome.has_value());
}
