// /provider add 向导(向导重排单):纯逻辑测试。脚本化导航事件(文本步)+
// 脚本化编号行(选择步)+ 假的 print 收集器 + 假的 fetch_models,不碰真实
// stdin/stdout,也不真发网络请求。步骤序:名字 → 接口格式 → base_url →
// 密钥来源 → 模型 → 推理档位 → 额外参数 → 确认。
//
// 文本步吃 events 队列(Submitted/Back/Cancelled/Eof 四态事件);选择步在
// 非交互回落下吃 lines 队列(编号)。两条队列分开,序列写得清楚。

#include <doctest/doctest.h>

#include <iostream>

#include "cli/provider_wizard.hpp"

using namespace lubancode;

namespace {

struct ScriptedIO {
    // 选择步(非交互编号回落)吃的行。
    std::vector<std::string> lines;
    std::size_t lines_next = 0;
    // 文本步吃的导航事件。
    std::vector<cli::WizardInputEvent> events;
    std::size_t events_next = 0;
    std::vector<std::string> printed;
    int fetch_calls = 0;
    std::vector<std::string> fetch_keys;
    cli::WizardFetchModelsFn fetch = [](config::Wire, const std::string&,
                                        const std::string&) -> std::expected<std::vector<api::ModelInfo>, api::Error> {
        return std::vector<api::ModelInfo>{};
    };

    void Say(const std::string& text) { events.push_back({cli::WizardInputEvent::Kind::Submitted, text}); }
    void Back() { events.push_back({cli::WizardInputEvent::Kind::Back, ""}); }
    void Cancel() { events.push_back({cli::WizardInputEvent::Kind::Cancelled, ""}); }

    cli::WizardIO Build() {
        cli::WizardIO io;
        io.print = [this](const std::string& line) { printed.push_back(line); };
        io.read_line = [this]() -> std::optional<std::string> {
            if (lines_next >= lines.size()) {
                return std::nullopt;
            }
            return lines[lines_next++];
        };
        io.read_event = [this]() -> cli::WizardInputEvent {
            if (events_next >= events.size()) {
                return cli::WizardInputEvent{cli::WizardInputEvent::Kind::Eof, std::string()};
            }
            return events[events_next++];
        };
        io.fetch_models = [this](config::Wire wire, const std::string& base_url,
                                 const std::string& api_key) {
            ++fetch_calls;  // 计数放包装层:自定义 fetch(不碰计数)也照数。
            fetch_keys.push_back(api_key);
            return fetch(wire, base_url, api_key);
        };
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

api::Error HttpError(int status, const std::string& body = "") {
    return api::Error{api::ErrorKind::HttpStatus, body, status};
}

}  // namespace

// ---------------------------------------------------------------------------
// 前进:八步走全
// ---------------------------------------------------------------------------

TEST_CASE("RunProviderAddWizard: 八步前进,inline key + 手填模型 + effort + extra_body,确认写入") {
    ScriptedIO scripted;
    scripted.lines = {"2", "3"};  // wire=responses, auth=inline
    scripted.Say("sub-openai");   // 1) 名字
    scripted.Say("https://cc.moontidef.work/v1/");  // 3) base_url,带尾斜杠
    scripted.Say("sk-test-key-1234567890");      // 4) inline key
    scripted.Say("gpt-5.5");                     // 5) model 手填
    scripted.Say("xhigh");                       // 6) effort
    scripted.Say("{\"thinking\":{\"type\":\"enabled\"},\"reasoning_effort\":\"max\"}");  // 7) extra_body
    scripted.Say("");                            // 8) 确认:回车 -> 默认写入
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});

    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.name == "sub-openai");
    CHECK(outcome->provider.base_url == "https://cc.moontidef.work/v1");  // 尾斜杠剥掉
    CHECK(outcome->provider.wire == config::Wire::Responses);
    CHECK(outcome->provider.auth == config::ProviderAuthMode::Inline);
    CHECK(outcome->provider.api_key == "sk-test-key-1234567890");
    CHECK(outcome->provider.model == "gpt-5.5");
    CHECK(outcome->provider.model_reasoning_effort == "xhigh");
    CHECK(outcome->provider.extra_body.at("reasoning_effort") == "max");
    CHECK(outcome->save_requested == true);
    CHECK(scripted.AnyPrintedContains("sk-test-..."));  // 汇总只露掩码
    CHECK_FALSE(scripted.AnyPrintedContains("sk-test-key-1234567890"));
    CHECK(scripted.fetch_calls == 0);  // 手填模型,不拉列表
}

TEST_CASE("RunProviderAddWizard: wire 先于 base_url——responses 的探测地址带 /models") {
    ScriptedIO scripted;
    scripted.lines = {"2", "2"};  // wire=responses, auth=env
    scripted.Say("p1");
    scripted.Say("https://api.example.test/v1");
    scripted.Say("");  // env 名:回车用默认(wire=responses -> OPENAI_API_KEY)
    scripted.Say("MyModel");
    scripted.Say("");
    scripted.Say("");
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.key_env == "OPENAI_API_KEY");
    CHECK(scripted.AnyPrintedContains("https://api.example.test/v1/models"));
}

TEST_CASE("RunProviderAddWizard: auth=env 且环境变量未设置时,模型步提前说清") {
    ScriptedIO scripted;
    scripted.lines = {"1", "2"};  // wire=anthropic, auth=env
    scripted.Say("p1");
    scripted.Say("https://api.example.test");
    scripted.Say("SOME_UNSET_ENV_FOR_TEST");
    scripted.Say("manual-model");  // 手填,不拉列表
    scripted.Say("");
    scripted.Say("");
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.key_env == "SOME_UNSET_ENV_FOR_TEST");
    CHECK(scripted.AnyPrintedContains("SOME_UNSET_ENV_FOR_TEST"));  // 未设置提示露过面
}

TEST_CASE("RunProviderAddWizard: auth=none,拉模型时 key 是空串(请求不带鉴权头)") {
    ScriptedIO scripted;
    scripted.fetch = [](config::Wire, const std::string&,
                        const std::string&) -> std::expected<std::vector<api::ModelInfo>, api::Error> {
        return std::vector<api::ModelInfo>{{"model-a", "Model A"}};
    };
    scripted.lines = {"2", "1", "1"};  // wire=responses, auth=none, 模型列表选 1
    scripted.Say("local");
    scripted.Say("http://127.0.0.1:8000/v1");
    scripted.Say("");   // 5) model:回车拉列表
    scripted.Say("");
    scripted.Say("");
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.auth == config::ProviderAuthMode::None);
    CHECK(outcome->provider.model == "model-a");
    REQUIRE(scripted.fetch_keys.size() == 1);
    CHECK(scripted.fetch_keys[0].empty());  // none:彻底不带 key
}

TEST_CASE("RunProviderAddWizard: 命令行给了合法名字就跳过名字步") {
    ScriptedIO scripted;
    scripted.lines = {"1", "3"};
    scripted.Say("https://api.example.test");
    scripted.Say("the-key");
    scripted.Say("MyModel");
    scripted.Say("");
    scripted.Say("");
    scripted.Say("n");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "myprovider", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.name == "myprovider");
    CHECK(outcome->save_requested == false);
}

// ---------------------------------------------------------------------------
// 校验:错误留在当前步,改正后过
// ---------------------------------------------------------------------------

TEST_CASE("RunProviderAddWizard: 名字输中文报错并给 slug 建议,改正后过") {
    ScriptedIO scripted;
    scripted.lines = {"1", "3"};
    scripted.Say("本地服务器");   // 不合规
    scripted.Say("local-server");  // 改正
    scripted.Say("https://api.example.test");
    scripted.Say("the-key");
    scripted.Say("MyModel");
    scripted.Say("");
    scripted.Say("");
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.name == "local-server");
    CHECK(scripted.AnyPrintedContains("local-server"));  // slug 建议露过面
}

TEST_CASE("RunProviderAddWizard: base_url 漏协议被拒,改正后过") {
    ScriptedIO scripted;
    scripted.lines = {"1", "3"};
    scripted.Say("p1");
    scripted.Say("not-a-url");                // 拒
    scripted.Say("ftp://example.test");       // 也拒
    scripted.Say("https://api.example.test"); // 过
    scripted.Say("the-key");
    scripted.Say("MyModel");
    scripted.Say("");
    scripted.Say("");
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.base_url == "https://api.example.test");
}

TEST_CASE("RunProviderAddWizard: OpenAI 兼容地址没带 /v1,给采用与否两个选项") {
    SUBCASE("采用 /v1") {
        ScriptedIO scripted;
        scripted.lines = {"2", "1", "1", "1"};  // wire=responses, v1 offer 采用, auth=none, 列表选 1
        scripted.Say("p1");
        scripted.Say("http://127.0.0.1:8000");
        scripted.fetch = [](config::Wire, const std::string&,
                            const std::string&) -> std::expected<std::vector<api::ModelInfo>, api::Error> {
            return std::vector<api::ModelInfo>{{"m", ""}};
        };
        scripted.Say("");  // model 回车拉列表
        scripted.Say("");
        scripted.Say("");
        scripted.Say("Y");
        auto io = scripted.Build();
        const auto outcome = cli::RunProviderAddWizard(io, "", {});
        REQUIRE(outcome.has_value());
        CHECK(outcome->provider.base_url == "http://127.0.0.1:8000/v1");
    }
    SUBCASE("保持原样") {
        ScriptedIO scripted;
        scripted.lines = {"2", "2", "1"};  // wire=responses, v1 offer 选 2(保持), auth=none
        scripted.Say("p1");
        scripted.Say("http://127.0.0.1:8000");
        scripted.Say("m");
        scripted.Say("");
        scripted.Say("");
        scripted.Say("Y");
        auto io = scripted.Build();
        const auto outcome = cli::RunProviderAddWizard(io, "", {});
        REQUIRE(outcome.has_value());
        CHECK(outcome->provider.base_url == "http://127.0.0.1:8000");
    }
}

TEST_CASE("RunProviderAddWizard: extra_body 坏 JSON 重问,合法 object 才过") {
    ScriptedIO scripted;
    scripted.lines = {"1", "3"};
    scripted.Say("p1");
    scripted.Say("https://api.example.test");
    scripted.Say("the-key");
    scripted.Say("MyModel");
    scripted.Say("");
    scripted.Say("{not json");   // 拒
    scripted.Say("[1,2,3]");     // 不是 object,拒
    scripted.Say("{\"a\":1}");   // 过
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.extra_body.at("a") == 1);
}

// ---------------------------------------------------------------------------
// 后退:每步能回,旧值作默认,模型列表作废重拉
// ---------------------------------------------------------------------------

TEST_CASE("RunProviderAddWizard: 模型步 Back 回密钥步,回车保留旧值,缓存不废") {
    ScriptedIO scripted;
    scripted.fetch = [](config::Wire, const std::string&,
                        const std::string&) -> std::expected<std::vector<api::ModelInfo>, api::Error> {
        return std::vector<api::ModelInfo>{{"model-a", ""}};
    };
    scripted.lines = {"1", "2", "2", "1"};
    //            wire, auth=env, 退回后再选 env, 列表选 1
    scripted.Say("p1");
    scripted.Say("https://a.test");
    scripted.Say("");    // env 名:默认 ANTHROPIC_AUTH_TOKEN
    scripted.Back();     // 模型步第一问 Back -> 回密钥步
    scripted.Say("");    // env 名:回车保留旧值
    scripted.Say("");    // model:回车拉列表(第 1 次)
    scripted.Say("");    // effort
    scripted.Say("");    // extra_body
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.key_env == "ANTHROPIC_AUTH_TOKEN");
    CHECK(outcome->provider.model == "model-a");
    CHECK(scripted.fetch_calls == 1);  // 退回再前进,值没改 -> 拉一次就够
}

TEST_CASE("RunProviderAddWizard: 汇总页跳回 base_url 改值,改完直回汇总") {
    ScriptedIO scripted;
    scripted.fetch = [](config::Wire, const std::string&,
                        const std::string&) -> std::expected<std::vector<api::ModelInfo>, api::Error> {
        return std::vector<api::ModelInfo>{{"model-a", ""}};
    };
    scripted.lines = {"1", "3", "1"};
    //            wire anthropic, auth=inline, 模型列表选 1
    scripted.Say("p1");
    scripted.Say("https://a.test");
    scripted.Say("the-key");
    scripted.Say("");    // model 回车拉列表(lines[2]=1 选 model-a)
    scripted.Say("");    // effort
    scripted.Say("");    // extra_body
    scripted.Say("3");   // 汇总页:跳回第 3 项 base_url
    scripted.Say("https://b.test");  // 改地址 -> AfterEdit 直回汇总,不重走 auth/model
    scripted.Say("Y");   // 汇总确认
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.base_url == "https://b.test");
    CHECK(outcome->provider.model == "model-a");
    CHECK(outcome->provider.name == "p1");
    CHECK(scripted.AnyPrintedContains("https://b.test"));  // 汇总里露出新地址
}

TEST_CASE("RunProviderAddWizard: 汇总页跳回 wire 改协议,模型缓存作废,前进时重拉") {
    ScriptedIO scripted;
    int fetch_count = 0;
    scripted.fetch = [&fetch_count](config::Wire wire, const std::string& base_url, const std::string&)
        -> std::expected<std::vector<api::ModelInfo>, api::Error> {
        ++fetch_count;
        (void)wire;
        (void)base_url;
        return std::vector<api::ModelInfo>{{"model-a", ""}};
    };
    scripted.lines = {"1", "3", "1", "2", "1"};
    //           wire anthropic, inline key, 列表选 1, 跳回 wire 后选 2, 列表再选 1
    scripted.Say("p1");
    scripted.Say("https://a.test");
    scripted.Say("the-key");
    scripted.Say("");    // model 回车拉列表(第 1 次)
    scripted.Say("");    // effort
    scripted.Say("");    // extra_body
    scripted.Say("2");   // 汇总页:跳回第 2 项 wire
    // wire 步:lines[3]="2" 选 responses -> 改完直回汇总
    scripted.Say("5");   // 汇总页:跳回第 5 项 model
    scripted.Say("");    // model 回车 -> 缓存已作废,重拉(第 2 次),lines[4]="1"
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.wire == config::Wire::Responses);
    CHECK(fetch_count == 2);  // 改了 wire,旧列表作废重拉
}

// ---------------------------------------------------------------------------
// 跳转:模型拉取失败页可直达 wire / base_url,404 可加 /v1 重试
// ---------------------------------------------------------------------------

TEST_CASE("RunProviderAddWizard: 拉取 404 可直达 base_url,改完再拉") {
    ScriptedIO scripted;
    scripted.fetch = [](config::Wire, const std::string& base_url,
                        const std::string&) -> std::expected<std::vector<api::ModelInfo>, api::Error> {
        if (base_url.find("a.test") != std::string::npos) {
            return std::unexpected(HttpError(404, "nope"));  // 旧地址 404,新地址出列表
        }
        return std::vector<api::ModelInfo>{{"model-a", ""}};
    };
    // lines: wire=responses(2), auth=none(1), 失败页选"返回检查 base_url"(选 3:
    // add_v1 不出现——地址已带 /v1;选项序 手填1/wire2/url3/重试4),改完前进
    // 再过一遍 auth(none),列表选 1。
    scripted.lines = {"2", "1", "3", "1", "1"};
    scripted.Say("p1");
    scripted.Say("https://a.test/v1");
    scripted.Say("");    // model 回车 -> 404 失败页
    scripted.Say("https://b.test/v1");  // base_url 步改地址 -> 前进回 auth
    scripted.Say("");    // model 再回车 -> 这次成功,lines[4]=1 选 model-a
    scripted.Say("");
    scripted.Say("");
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.base_url == "https://b.test/v1");
    CHECK(outcome->provider.model == "model-a");
    CHECK(scripted.fetch_calls == 2);
    CHECK(scripted.AnyPrintedContains("404"));
}

TEST_CASE("RunProviderAddWizard: OpenAI 地址没 /v1 又吃 404,可选'加上 /v1 后重试'") {
    ScriptedIO scripted;
    bool fail = true;
    scripted.fetch = [&fail](config::Wire wire, const std::string& base_url,
                             const std::string&) -> std::expected<std::vector<api::ModelInfo>, api::Error> {
        (void)wire;
        if (fail && base_url.find("/v1") == std::string::npos) {
            return std::unexpected(HttpError(404));
        }
        fail = false;
        return std::vector<api::ModelInfo>{{"m", ""}};
    };
    // lines: wire=responses(2), auth=none(1), 失败页第 1 项 = 加 /v1 重试, 列表选 1
    scripted.lines = {"2", "1", "1", "1"};
    scripted.Say("p1");
    scripted.Say("http://127.0.0.1:8000");
    scripted.Say("");  // model 回车 -> 404(地址没 /v1,失败页多一项)
    scripted.Say("");
    scripted.Say("");
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.base_url == "http://127.0.0.1:8000/v1");
    CHECK(outcome->provider.model == "m");
    CHECK(scripted.AnyPrintedContains("http://127.0.0.1:8000/v1/models"));
}

TEST_CASE("RunProviderAddWizard: 拉取失败仍可手填模型名,空串报错留在本步") {
    ScriptedIO scripted;
    scripted.fetch = [](config::Wire, const std::string&,
                        const std::string&) -> std::expected<std::vector<api::ModelInfo>, api::Error> {
        return std::unexpected(api::Error{api::ErrorKind::Network, "connect refused", 0});
    };
    // lines: wire=1, auth=3(inline), 失败页选手填(1)
    scripted.lines = {"1", "3", "1"};
    scripted.Say("p1");
    scripted.Say("https://a.test");
    scripted.Say("the-key");
    scripted.Say("");    // model 回车 -> 网络失败
    scripted.Say("");    // 手填空串 -> 报错,留在本步
    scripted.Say("m1");  // 填上 -> 过
    scripted.Say("");
    scripted.Say("");
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.model == "m1");
    CHECK(scripted.AnyPrintedContains("连接失败"));
}

// ---------------------------------------------------------------------------
// 取消:EOF / Ctrl+C / 第一步再退的确认
// ---------------------------------------------------------------------------

TEST_CASE("RunProviderAddWizard: 事件队列耗尽(EOF)返回 nullopt") {
    ScriptedIO scripted;
    scripted.lines = {"1"};
    scripted.Say("p1");
    scripted.Say("https://a.test");
    // wire 之后没有事件了 -> EOF
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    CHECK_FALSE(outcome.has_value());
}

TEST_CASE("RunProviderAddWizard: Ctrl+C 事件直接取消,不写盘") {
    ScriptedIO scripted;
    scripted.lines = {"1", "1"};
    scripted.Say("p1");
    scripted.Say("https://a.test");
    scripted.Say("");  // model 回车
    scripted.Cancel();
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    CHECK_FALSE(outcome.has_value());
}

TEST_CASE("RunProviderAddWizard: 第一步 Back 弹退出确认,回车默认不退") {
    ScriptedIO scripted;
    scripted.lines = {"1", "3"};
    scripted.Back();   // 名字步 Back -> 退出确认
    scripted.Say("");  // 默认 N:不退,留在名字步
    scripted.Say("p1");
    scripted.Say("https://a.test");
    scripted.Say("the-key");
    scripted.Say("m");
    scripted.Say("");
    scripted.Say("");
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.name == "p1");
    CHECK(scripted.AnyPrintedContains("退出向导"));
}

TEST_CASE("RunProviderAddWizard: 第一步 Back 后答 y,退出向导") {
    ScriptedIO scripted;
    scripted.Back();
    scripted.Say("y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    CHECK_FALSE(outcome.has_value());
}

TEST_CASE("RunProviderAddWizard: 回到密钥步不回显明文 key") {
    ScriptedIO scripted;
    scripted.lines = {"1", "3", "3"};
    scripted.Say("p1");
    scripted.Say("https://a.test");
    scripted.Say("sk-secret-key-9876543210");
    scripted.Say("m");
    scripted.Say("");
    scripted.Say("");
    scripted.Say("4");   // 汇总页跳回第 4 项 auth
    // auth 步:lines[2]="3" 再选 inline -> 子页显示"已设置明文密钥(掩码)"
    scripted.Say("");    // 回车保留旧 key
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderAddWizard(io, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.api_key == "sk-secret-key-9876543210");
    // 全程打印里不许出现明文 key(汇总/回显都只露掩码——掩码=前 8 位)。
    CHECK_FALSE(scripted.AnyPrintedContains("sk-secret-key-9876543210"));
    CHECK(scripted.AnyPrintedContains("sk-secre"));
}

// ---------------------------------------------------------------------------
// 预设向导
// ---------------------------------------------------------------------------

TEST_CASE("RunProviderPresetWizard: 选预设只问密钥与确认,参数全带上") {
    const auto catalog = config::ParseProviderCatalogJson(
        R"({"schema_version":1,"revision":"2026-07-25","providers":{"glm":{"name":"GLM","description":"Chat","wire":"chat_completions","base_url":"https://api.test/v1","key_env":"GLM_KEY","default_model":"glm-x","model_reasoning_effort":"max","extra_body":{"tool_stream":true},"models":{"glm-x":{"name":"GLM X","context_window":"1m"}}}}})",
        "p");
    REQUIRE(catalog.has_value());
    ScriptedIO scripted;
    scripted.lines = {"1", "3"};  // 目录选 glm, auth=inline
    scripted.Say("sk-demo");      // inline key
    scripted.Say("");             // 确认:回车写入
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderPresetWizard(io, *catalog, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->save_requested);
    CHECK(outcome->provider.name == "glm");
    CHECK(outcome->provider.wire == config::Wire::ChatCompletions);
    CHECK(outcome->provider.model == "glm-x");
    CHECK(outcome->provider.context_window_tokens == 1000000);
    CHECK(outcome->provider.auth == config::ProviderAuthMode::Inline);
    CHECK(outcome->provider.api_key == "sk-demo");
    CHECK(outcome->provider.extra_body["tool_stream"] == true);
    CHECK(outcome->provider.key_env == "GLM_KEY");
}

TEST_CASE("RunProviderPresetWizard: 预设也认'无需鉴权'") {
    const auto catalog = config::ParseProviderCatalogJson(
        R"({"schema_version":1,"revision":"2026-07-25","providers":{"p":{"name":"P","wire":"responses","base_url":"https://api.test/v1","key_env":"P_KEY","default_model":"m","models":{"m":{"name":"M"}}}}})",
        "p");
    REQUIRE(catalog.has_value());
    ScriptedIO scripted;
    scripted.lines = {"1", "1"};  // 目录选 p, auth=none
    scripted.Say("");             // 确认
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderPresetWizard(io, *catalog, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.auth == config::ProviderAuthMode::None);
    CHECK(outcome->provider.model == "m");
}

TEST_CASE("RunProviderPresetWizard: 末项回到全手填向导(新次序)") {
    const auto catalog = config::ParseProviderCatalogJson(
        R"({"schema_version":1,"revision":"2026-07-25","providers":{"p":{"name":"P","wire":"responses","base_url":"https://api.test/v1","key_env":"P_KEY","default_model":"m","models":{"m":{"name":"M"}}}}})",
        "p");
    REQUIRE(catalog.has_value());
    ScriptedIO scripted;
    scripted.lines = {"2", "3", "3"};  // 目录选自定义, wire=chat(3), auth=inline(3)
    scripted.Say("custom");
    scripted.Say("https://custom.test/v1");
    scripted.Say("key");
    scripted.Say("model");
    scripted.Say("");
    scripted.Say("");
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderPresetWizard(io, *catalog, "", {});
    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.name == "custom");
    CHECK(outcome->provider.wire == config::Wire::ChatCompletions);
}

// ---------------------------------------------------------------------------
// 纯小工具
// ---------------------------------------------------------------------------

TEST_CASE("SuggestProviderSlug: 空格折短横线,中文折一枚,全废抽空串") {
    CHECK(cli::SuggestProviderSlug("local server") == "local-server");
    CHECK(cli::SuggestProviderSlug("本地服务器") == "");
    CHECK(cli::SuggestProviderSlug("my 127.0.0.1 服务") == "my-127.0.0.1");
    CHECK(cli::SuggestProviderSlug("a/b c") == "a-b-c");
}

TEST_CASE("IsLocalBaseUrl: 本机与局域网认得,公网不认") {
    CHECK(cli::IsLocalBaseUrl("http://127.0.0.1:8000/v1"));
    CHECK(cli::IsLocalBaseUrl("http://localhost:3000"));
    CHECK(cli::IsLocalBaseUrl("http://192.168.1.5/v1"));
    CHECK(cli::IsLocalBaseUrl("http://10.0.0.2/v1"));
    CHECK_FALSE(cli::IsLocalBaseUrl("https://api.minimax.io"));
    CHECK_FALSE(cli::IsLocalBaseUrl("https://cc.moontidef.work/v1"));
}

// ---------------------------------------------------------------------------
// /provider edit(容错单):同一套向导面板改旧 provider。夹具照 add 的五组
// (前进/后退/跳转/失效/取消)对 edit 模式照跑,另钉 diff、掩码与锁名。
// ---------------------------------------------------------------------------

namespace {

config::ProviderConfig EditableProvider() {
    config::ProviderConfig p;
    p.name = "custom";
    p.base_url = "https://old.example.test/v1";
    p.wire = config::Wire::Responses;
    p.auth = config::ProviderAuthMode::Inline;
    p.api_key = "sk-old-key-123456";
    p.model = "old-model";
    p.model_reasoning_effort = "high";
    p.extra_body["thinking"] = true;
    p.context_window_tokens = 200000;   // 向导不碰的字段:写盘得原样带回去
    p.native_web_search = true;
    return p;
}

}  // namespace

TEST_CASE("RunProviderEditWizard: 跳转——起手汇总页,跳回 base_url 改值,直回汇总,确认写入") {
    ScriptedIO scripted;
    scripted.Say("3");                            // 汇总页跳第 3 项 base_url
    scripted.Say("https://new.example.test/v1");  // 改地址 -> 回程票直回汇总
    scripted.Say("Y");                            // 确认写入
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderEditWizard(io, EditableProvider());

    REQUIRE(outcome.has_value());
    CHECK(outcome->save_requested);
    CHECK(outcome->provider.name == "custom");
    CHECK(outcome->provider.base_url == "https://new.example.test/v1");
    // 其他字段原样:改一项不动全身。
    CHECK(outcome->provider.model == "old-model");
    CHECK(outcome->provider.model_reasoning_effort == "high");
    CHECK(outcome->provider.api_key == "sk-old-key-123456");
    CHECK(outcome->provider.context_window_tokens == 200000);
    CHECK(outcome->provider.native_web_search);
    CHECK(outcome->provider.extra_body.at("thinking") == true);
    CHECK(scripted.fetch_calls == 0);  // 编辑模式不拉模型列表
    // 确认页 diff:旧值 -> 新值都露面。
    CHECK(scripted.AnyPrintedContains("https://old.example.test/v1 → https://new.example.test/v1"));
}

TEST_CASE("RunProviderEditWizard: 前进——回车逐项保留,一个不改,原样写回") {
    ScriptedIO scripted;
    scripted.Say("3");   // 跳回 base_url
    scripted.Say("");    // 回车 = 保留旧值
    scripted.Say("Y");   // 汇总确认
    auto io = scripted.Build();
    const config::ProviderConfig original = EditableProvider();
    const auto outcome = cli::RunProviderEditWizard(io, original);

    REQUIRE(outcome.has_value());
    CHECK(outcome->save_requested);
    CHECK(outcome->provider.base_url == original.base_url);
    CHECK(outcome->provider.model == original.model);
    CHECK(outcome->provider.api_key == original.api_key);
    CHECK(scripted.AnyPrintedContains("本次没有字段改动"));
}

TEST_CASE("RunProviderEditWizard: 后退——从 effort 一路退回 auth 再前进,全程保留旧值") {
    ScriptedIO scripted;
    scripted.lines = {"3"};  // auth 选择页选 3(inline,保持原模式)
    scripted.Say("6");       // 汇总跳第 6 项 effort
    scripted.Back();         // effort -> 模型步(回程票被 Back 撕掉)
    scripted.Back();         // 模型步 -> 密钥步(选择页,吃 lines[0])
    scripted.Say("");        // inline 子页:回车保留旧 key
    scripted.Say("");        // 模型步:回车保留(edit 模式不拉列表)
    scripted.Say("");        // effort:回车保留
    scripted.Say("");        // extra_body:回车保留
    scripted.Say("Y");       // 汇总确认
    auto io = scripted.Build();
    const config::ProviderConfig original = EditableProvider();
    const auto outcome = cli::RunProviderEditWizard(io, original);

    REQUIRE(outcome.has_value());
    CHECK(outcome->save_requested);
    CHECK(outcome->provider.auth == config::ProviderAuthMode::Inline);
    CHECK(outcome->provider.api_key == "sk-old-key-123456");  // 退回再前进,key 不丢
    CHECK(outcome->provider.model == original.model);
    CHECK(outcome->provider.model_reasoning_effort == "high");
    CHECK(scripted.fetch_calls == 0);
    // 回到密钥步不回显明文,只露掩码。
    CHECK_FALSE(scripted.AnyPrintedContains("sk-old-key-123456"));
}

TEST_CASE("RunProviderEditWizard: 失效——改了地址也不发请求,模型步回车保留,手改才落") {
    ScriptedIO scripted;
    scripted.Say("3");                            // 改 base_url(地址变了,旧模型列表作废)
    scripted.Say("https://new.example.test/v1");
    scripted.Say("5");                            // 汇总跳第 5 项 model
    scripted.Say("new-model");                    // 手敲新模型
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderEditWizard(io, EditableProvider());

    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.base_url == "https://new.example.test/v1");
    CHECK(outcome->provider.model == "new-model");
    CHECK(scripted.fetch_calls == 0);  // 地址作废也轮不到拉列表:edit 模式压根不发请求
}

TEST_CASE("RunProviderEditWizard: 取消——Ctrl+C 与 EOF 都返回 nullopt,不写盘") {
    SUBCASE("汇总页 Ctrl+C") {
        ScriptedIO scripted;
        scripted.Cancel();
        auto io = scripted.Build();
        CHECK_FALSE(cli::RunProviderEditWizard(io, EditableProvider()).has_value());
    }
    SUBCASE("改到一半 EOF") {
        ScriptedIO scripted;
        scripted.Say("3");  // 跳回 base_url,之后事件耗尽 -> EOF
        auto io = scripted.Build();
        CHECK_FALSE(cli::RunProviderEditWizard(io, EditableProvider()).has_value());
    }
    SUBCASE("最后一问答 n,不写盘") {
        ScriptedIO scripted;
        scripted.Say("n");
        auto io = scripted.Build();
        const auto outcome = cli::RunProviderEditWizard(io, EditableProvider());
        REQUIRE(outcome.has_value());
        CHECK_FALSE(outcome->save_requested);
    }
}

TEST_CASE("RunProviderEditWizard: 换 key 走 diff,明文只在内存里,屏上全是掩码") {
    ScriptedIO scripted;
    scripted.lines = {"3"};             // auth 选择页选 3(inline)
    scripted.Say("4");                  // 汇总跳第 4 项 auth
    scripted.Say("sk-new-key-999888");  // 贴新 key
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderEditWizard(io, EditableProvider());

    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.api_key == "sk-new-key-999888");
    // 全程打印里不许出现新旧明文,只许掩码(前 8 位)。
    CHECK_FALSE(scripted.AnyPrintedContains("sk-old-key-123456"));
    CHECK_FALSE(scripted.AnyPrintedContains("sk-new-key-999888"));
    CHECK(scripted.AnyPrintedContains("sk-old-k"));
    CHECK(scripted.AnyPrintedContains("sk-new-k"));
    CHECK(scripted.AnyPrintedContains("→"));  // diff 行露过面
}

TEST_CASE("RunProviderEditWizard: env 模式回车保留自定义变量名,不悄悄复位默认值") {
    config::ProviderConfig provider = EditableProvider();
    provider.auth = config::ProviderAuthMode::Env;
    provider.api_key.clear();
    provider.key_env = "MY_CUSTOM_KEY";

    ScriptedIO scripted;
    scripted.lines = {"2"};  // auth 选择页选 2(env)
    scripted.Say("4");       // 汇总跳第 4 项 auth
    scripted.Say("");        // env 子页:回车 = 保留 MY_CUSTOM_KEY
    scripted.Say("Y");
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderEditWizard(io, provider);

    REQUIRE(outcome.has_value());
    CHECK(outcome->provider.key_env == "MY_CUSTOM_KEY");
}

TEST_CASE("RunProviderEditWizard: 汇总页第 1 项锁名,明说不支持改名") {
    ScriptedIO scripted;
    scripted.Say("1");  // 想跳去改名字
    scripted.Say("Y");  // 被拦回汇总后确认写入
    auto io = scripted.Build();
    const auto outcome = cli::RunProviderEditWizard(io, EditableProvider());

    REQUIRE(outcome.has_value());
    CHECK(outcome->save_requested);
    CHECK(outcome->provider.name == "custom");  // 名字没动
    CHECK(scripted.AnyPrintedContains("不支持改名"));
}

TEST_CASE("ProviderEditDiffLines: 改哪行哪行带箭头,没改的原样,全没改附说明") {
    const config::ProviderConfig original = EditableProvider();

    SUBCASE("只改 base_url") {
        config::ProviderConfig draft = original;
        draft.base_url = "https://new.example.test/v1";
        const auto lines = cli::ProviderEditDiffLines(original, draft);
        REQUIRE(lines.size() == 7);  // 7 行汇总;有改动就不附"没有改动"说明
        CHECK(lines[0].find("custom") != std::string::npos);
        CHECK(lines[0].find("不支持改名") != std::string::npos);
        bool diff_seen = false;
        for (const std::string& line : lines) {
            const bool has_arrow = line.find("→") != std::string::npos;
            if (line.find("base_url") != std::string::npos) {
                CHECK(has_arrow);
                CHECK(line.find("https://old.example.test/v1") != std::string::npos);
                CHECK(line.find("https://new.example.test/v1") != std::string::npos);
                diff_seen = true;
            } else {
                CHECK_FALSE(has_arrow);  // 别的行不许带箭头
            }
        }
        CHECK(diff_seen);
        // key 只露掩码,明文不进 diff 行。
        for (const std::string& line : lines) {
            CHECK(line.find("sk-old-key-123456") == std::string::npos);
        }
    }

    SUBCASE("一个没改") {
        const auto lines = cli::ProviderEditDiffLines(original, original);
        REQUIRE(lines.size() == 8);  // 7 行汇总 + 1 行"本次没有字段改动"
        CHECK(lines.back().find("本次没有字段改动") != std::string::npos);
        for (const std::string& line : lines) {
            CHECK(line.find("→") == std::string::npos);
        }
    }
}

TEST_CASE("ProviderWizardStepIndex: 八步序是枚举序") {
    CHECK(cli::ProviderWizardStepIndex(cli::ProviderWizardStep::Name) == 0);
    CHECK(cli::ProviderWizardStepIndex(cli::ProviderWizardStep::Wire) == 1);
    CHECK(cli::ProviderWizardStepIndex(cli::ProviderWizardStep::BaseUrl) == 2);
    CHECK(cli::ProviderWizardStepIndex(cli::ProviderWizardStep::Auth) == 3);
    CHECK(cli::ProviderWizardStepIndex(cli::ProviderWizardStep::Model) == 4);
    CHECK(cli::ProviderWizardStepIndex(cli::ProviderWizardStep::Effort) == 5);
    CHECK(cli::ProviderWizardStepIndex(cli::ProviderWizardStep::ExtraBody) == 6);
    CHECK(cli::ProviderWizardStepIndex(cli::ProviderWizardStep::Confirm) == 7);
    CHECK(cli::kProviderWizardStepCount == 8);
    CHECK(cli::ProviderWizardStepAt(99) == cli::ProviderWizardStep::Confirm);
}
