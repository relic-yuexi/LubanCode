// /provider switch 选择器(向导重排单):条目构建/过滤/按键状态机是纯逻辑,
// 这里脱离终端单测。RunProviderSwitchPicker 的 TTY 面板与缺密钥补救页是
// 交互路径,归真机手测;解析层的 SwitchInteractive 判定在 test_slash_commands。

#include <doctest/doctest.h>

#include "cli/i18n.hpp"
#include "cli/provider_switch.hpp"

using namespace lubancode;

namespace {

config::ProviderConfig MakeProvider(const std::string& name, const std::string& url,
                                    config::ProviderAuthMode auth, const std::string& key_env = "",
                                    const std::string& model = "m") {
    config::ProviderConfig provider;
    provider.name = name;
    provider.base_url = url;
    provider.auth = auth;
    provider.key_env = key_env;
    provider.model = model;
    return provider;
}

}  // namespace

TEST_CASE("BuildProviderSwitchEntries: 名字/模型/短地址/鉴权状态/当前标记,不带明文") {
    std::vector<config::ProviderConfig> providers = {
        MakeProvider("local", "http://127.0.0.1:8000/v1", config::ProviderAuthMode::None, "", "gpt-5.5"),
        MakeProvider("minimax", "https://api.minimax.io/anthropic", config::ProviderAuthMode::Env,
                     "MINIMAX_KEY"),
        MakeProvider("custom", "https://cc.example.test/v1", config::ProviderAuthMode::Inline, "",
                     "MyModel"),
    };
    providers[2].api_key = "sk-plaintext-key-123";

    const auto entries = cli::BuildProviderSwitchEntries(providers, "minimax");
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].name == "local");
    CHECK(entries[0].short_url == "127.0.0.1:8000/v1");
    CHECK(entries[0].model == "gpt-5.5");
    CHECK(entries[0].auth_label == cli::tr("cmd.provider.auth_none"));  // none -> 无需鉴权
    CHECK_FALSE(entries[0].is_current);

    CHECK(entries[1].is_current);  // 当前标记
    // MINIMAX_KEY 在测试环境没设:按"缺密钥(需要 X)"标,不谎报可用。
    CHECK(entries[1].auth_label.find("MINIMAX_KEY") != std::string::npos);

    CHECK(entries[2].auth_label == cli::tr("provider_switch.auth_ready"));  // inline 且有 key -> 可用
}

TEST_CASE("BuildProviderSwitchEntries: inline 缺 key 标'缺明文 key',不回显明文") {
    std::vector<config::ProviderConfig> providers = {
        MakeProvider("half", "https://h.test/v1", config::ProviderAuthMode::Inline, "", "m"),
    };
    providers[0].api_key.clear();
    const auto entries = cli::BuildProviderSwitchEntries(providers, "");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].auth_label == cli::tr("provider_switch.auth_inline_missing"));
}

TEST_CASE("ShortenProviderUrl: 剥协议头,长路径只留一段") {
    CHECK(cli::ShortenProviderUrl("http://127.0.0.1:8000/v1") == "127.0.0.1:8000/v1");
    CHECK(cli::ShortenProviderUrl("https://api.minimax.io/anthropic") == "api.minimax.io/anthropic");
    CHECK(cli::ShortenProviderUrl("https://api.example.test/") == "api.example.test");
    CHECK(cli::ShortenProviderUrl("https://api.example.test") == "api.example.test");
    // 两段以上的路径只留第一段。
    CHECK(cli::ShortenProviderUrl("https://x.test/a/b/c") == "x.test/a");
}

TEST_CASE("FilterProviderSwitchEntries: 按名字/模型/地址过滤,大小写不敏感") {
    const std::vector<config::ProviderConfig> providers = {
        MakeProvider("local", "http://127.0.0.1:8000/v1", config::ProviderAuthMode::None, "", "gpt-5.5"),
        MakeProvider("minimax", "https://api.minimax.io", config::ProviderAuthMode::Env, "K", "MiniMax-M2.5"),
        MakeProvider("custom", "https://cc.example.test/v1", config::ProviderAuthMode::Inline, "", "m"),
    };
    const auto entries = cli::BuildProviderSwitchEntries(providers, "");

    CHECK(cli::FilterProviderSwitchEntries(entries, "").size() == 3);   // 空滤=全保留
    // "mini" 同时命中 minimax 的名字与模型,但那是同一家,只留一条。
    CHECK(cli::FilterProviderSwitchEntries(entries, "mini").size() == 1);
    CHECK(cli::FilterProviderSwitchEntries(entries, "127.0.0.1").size() == 1);
    CHECK(cli::FilterProviderSwitchEntries(entries, "LOCAL").size() == 1);  // 大小写不敏感
    CHECK(cli::FilterProviderSwitchEntries(entries, "no-such").empty());
}

TEST_CASE("ProviderSwitchCore: 字符进筛选词,Backspace 退格,Enter 选中,Esc 取消") {
    cli::ProviderSwitchCore core(3, 1);
    CHECK(core.state().cursor == 1);

    core.HandleKey(cli::KeyEvent::Simple(cli::KeyKind::Up));
    CHECK(core.state().cursor == 0);
    core.HandleKey(cli::KeyEvent::Simple(cli::KeyKind::Up));
    CHECK(core.state().cursor == 2);  // 环绕

    core.HandleKey(cli::KeyEvent::Simple(cli::KeyKind::Down));
    CHECK(core.state().cursor == 0);

    // 中文字符也进得了筛选词(UTF-8 编码进去)。
    core.HandleKey(cli::KeyEvent::Char(U'本'));
    core.HandleKey(cli::KeyEvent::Char(U'地'));
    CHECK(core.state().filter == "本地");
    core.HandleKey(cli::KeyEvent::Simple(cli::KeyKind::Backspace));
    CHECK(core.state().filter == "本");

    core.HandleKey(cli::KeyEvent::Simple(cli::KeyKind::Enter));
    CHECK(core.state().submitted);
    CHECK_FALSE(core.state().cancelled);
}

TEST_CASE("ProviderSwitchCore: SetVisibleCount 把光标钳进列表") {
    cli::ProviderSwitchCore core(5, 4);
    CHECK(core.state().cursor == 4);
    core.SetVisibleCount(2);  // 过滤后只剩两项
    CHECK(core.state().cursor == 1);
    core.SetVisibleCount(0);
    CHECK(core.state().cursor == 0);
    // 空列表按 Enter 不算选中。
    core.HandleKey(cli::KeyEvent::Simple(cli::KeyKind::Enter));
    CHECK_FALSE(core.state().submitted);
}

TEST_CASE("ProviderSwitchCore: Esc/Ctrl+C 取消") {
    cli::ProviderSwitchCore core(2, 0);
    core.HandleKey(cli::KeyEvent::Simple(cli::KeyKind::Esc));
    CHECK(core.state().cancelled);
    CHECK_FALSE(core.state().submitted);

    cli::ProviderSwitchCore core2(2, 0);
    core2.HandleKey(cli::KeyEvent::Simple(cli::KeyKind::CtrlC));
    CHECK(core2.state().cancelled);
}
