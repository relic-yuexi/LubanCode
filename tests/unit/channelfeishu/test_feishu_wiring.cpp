// 飞书装配接线册(飞书/企微设计单 F1):ChannelGatewayWiring::Create 对
// feishu 注册行的真装配——五闸裁决、账号进 ChannelManager 并起跑;注册表/
// setup 平台表/模板三处同源(feishu 进"认得"清单)。传输与 HTTP 全注假,
// 零外联。
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/channel_adapter_registry.hpp"
#include "app/channel_gateway_wiring.hpp"
#include "channel/channel_setup.hpp"
#include "channel/feishu/feishu_gateway.hpp"
#include "config/config.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::app {
namespace {

// 静默假网关传输:连接即过,读恒超时(网关线程在退避循环里安静转)。
class QuietFeishuTransport final : public channel::feishu::IFeishuGatewayTransport {
public:
    std::expected<void, channel::feishu::FeishuConnectError> Connect(const std::string&) override {
        return {};
    }
    std::expected<void, std::string> SendBinary(const std::string&) override { return {}; }
    std::expected<std::string, channel::transport::WsError> ReadMessage(int) override {
        return std::unexpected(channel::transport::WsError{
            channel::transport::WsError::Kind::Timeout, "quiet", 0});
    }
    void Cancel() override {}
    void Close(std::uint16_t, const std::string&) override {}
};

channel::feishu::FeishuHttpFunc QuietFeishuHttp() {
    return [](const channel::feishu::FeishuHttpRequest&)
        -> std::expected<channel::feishu::FeishuHttpResponse, std::string> {
        return std::unexpected("quiet test http");
    };
}

// qq 静默传输(混配案用;形状与 test_qq_wiring 的 QuietGatewayTransport 同)。
class QuietQqTransport final : public channel::qq::IGatewayTransport {
public:
    std::expected<void, channel::qq::GatewayConnectError> Connect(const std::string&) override {
        return {};
    }
    std::expected<void, std::string> SendText(const std::string&) override { return {}; }
    std::expected<std::string, channel::transport::WsError> ReadMessage(int) override {
        return std::unexpected(channel::transport::WsError{
            channel::transport::WsError::Kind::Timeout, "quiet", 0});
    }
    void Cancel() override {}
    void Close(std::uint16_t, const std::string&) override {}
};

std::filesystem::path MakeTempRoot(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-feishu-wiring-test-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

ChannelGatewayWiring::Options MakeOptions(const config::Config* config,
                                          const std::filesystem::path& root) {
    ChannelGatewayWiring::Options options;
    options.config = config;
    options.channels_state_root = root;
    options.now_ms = []() { return platform::WallClockNowMs(); };
    options.test_feishu_transport_factory = []() {
        return std::unique_ptr<channel::feishu::IFeishuGatewayTransport>(
            std::make_unique<QuietFeishuTransport>());
    };
    options.test_feishu_http = QuietFeishuHttp();
    // qq 注入位也压静默(混配渠道账的案里 qq 账号不该真连)。
    options.test_transport_factory = []() {
        return std::unique_ptr<channel::qq::IGatewayTransport>(
            std::make_unique<QuietQqTransport>());
    };
    options.test_http = [](const channel::qq::QqHttpRequest&)
        -> std::expected<channel::qq::QqHttpResponse, std::string> {
        return std::unexpected("quiet test http");
    };
    return options;
}

}  // namespace

TEST_CASE("feishu_wiring: 注册表/setup 平台表/模板三处同源") {
    // 注册表:feishu 有注册行(im 文案"认得"清单的真源)。
    const std::vector<std::string>& implemented = ImplementedChannelAdapterIds();
    bool saw_feishu = false;
    bool saw_qq = false;
    for (const std::string& id : implemented) {
        if (id == "feishu") saw_feishu = true;
        if (id == "qqbot") saw_qq = true;
    }
    CHECK(saw_feishu);
    CHECK(saw_qq);
    CHECK(ChannelAdapterRegistry().count("feishu") == 1);

    // setup 平台表:feishu implemented=true,凭据字段 App ID/App Secret。
    const auto platform = channel::FindChannelSetupPlatform("feishu");
    REQUIRE(platform.has_value());
    CHECK(platform->implemented);
    CHECK(platform->display_name == "飞书");
    REQUIRE(platform->fields.size() == 2);
    CHECK(platform->fields[0].id == "app_id");
    CHECK(platform->fields[0].label == "App ID");
    CHECK_FALSE(platform->fields[0].sensitive);
    CHECK(platform->fields[1].id == "app_secret");
    CHECK(platform->fields[1].label == "App Secret");
    CHECK(platform->fields[1].sensitive);

    // 模板:照 QQ 五可选项对齐 + secret_env 预指。
    const channel::ChannelAccountUserConfig template_account =
        channel::MakeFeishuTemplateAccount();
    CHECK(template_account.transport == "websocket");
    CHECK(template_account.dm_policy == channel::DmPolicy::Pairing);
    CHECK(template_account.group_policy == channel::GroupPolicy::Disabled);
    CHECK_FALSE(template_account.allow_bots);
    CHECK(template_account.require_mention);
    CHECK(template_account.reply.mode == channel::ReplyMode::Final);
    REQUIRE(template_account.secret_env.has_value());
    CHECK(*template_account.secret_env == "FEISHU_APP_SECRET");
    CHECK_FALSE(template_account.enabled);  // 用户配齐凭据再自己开

    // 配置解析:feishu 渠道段按通用 schema 解析(与 qq 同一份解析器)。
    std::string error;
    const auto parsed = channel::ParseChannelsUserConfig(nlohmann::json::parse(R"({
        "feishu": {
            "enabled": true,
            "default_account": "work",
            "accounts": {
                "work": {
                    "enabled": true,
                    "transport": "websocket",
                    "app_id": "cli_app1",
                    "secret_env": "FEISHU_APP_SECRET",
                    "dm_policy": "pairing",
                    "group_policy": "disabled",
                    "reply": {"mode": "final"}
                }
            }
        }
    })"), "wiring-test.json", &error);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->count("feishu") == 1);
    REQUIRE(parsed->at("feishu").accounts.count("work") == 1);
    CHECK(parsed->at("feishu").accounts.at("work").app_id == "cli_app1");
}

TEST_CASE("feishu_wiring: 五闸全过——feishu 账号真装配进 ChannelManager 并起跑") {
    const auto root = MakeTempRoot("ready");
    config::Config config;
    channel::ChannelUserConfig feishu;
    feishu.enabled = true;
    channel::ChannelAccountUserConfig account = channel::MakeFeishuTemplateAccount();
    account.enabled = true;
    account.app_id = "cli_app1";
    account.secret = std::string("inline-secret");
    feishu.accounts["work"] = account;
    config.channels["feishu"] = feishu;

    auto wiring = ChannelGatewayWiring::Create(MakeOptions(&config, root));
    REQUIRE(wiring != nullptr);
    CHECK(wiring->skipped().empty());
    CHECK(wiring->adapter_count() == 1);
    REQUIRE(wiring->manager() != nullptr);
    const auto snapshots = wiring->manager()->Snapshots();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].channel_id == "feishu");
    CHECK(snapshots[0].account_id == "work");
    // 装配后 StartAccount 已发;TickOnce 推进桥泵直到 Running。
    const auto deadline = platform::WallClockNowMs() + 8'000;
    while (platform::WallClockNowMs() < deadline) {
        (void)wiring->TickOnce(platform::WallClockNowMs());
        const auto current = wiring->manager()->Snapshots();
        if (!current.empty() && current[0].state == channel::ChannelAccountState::Running) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto final_snapshots = wiring->manager()->Snapshots();
    CHECK(final_snapshots[0].state == channel::ChannelAccountState::Running);
    CHECK(wiring->Close(5'000));
    wiring.reset();
}

TEST_CASE("feishu_wiring: 分支裁决——渠道禁用/账号禁用/凭据缺失/未注册渠道") {
    const auto root = MakeTempRoot("branches");
    config::Config config;
    channel::ChannelUserConfig feishu;
    feishu.enabled = false;  // 渠道禁用
    channel::ChannelAccountUserConfig account = channel::MakeFeishuTemplateAccount();
    account.enabled = true;
    account.secret = std::string("inline-secret");
    feishu.accounts["work"] = account;
    config.channels["feishu"] = feishu;
    // 未注册渠道(wecombot 属 W1 批次,还没注册行)。
    config.channels["wecombot"] = channel::ChannelUserConfig{};

    auto wiring = ChannelGatewayWiring::Create(MakeOptions(&config, root));
    REQUIRE(wiring != nullptr);
    CHECK(wiring->adapter_count() == 0);
    bool saw_channel_disabled = false;
    bool saw_unimplemented = false;
    for (const std::string& line : wiring->skipped()) {
        if (line.find("channel_disabled") != std::string::npos) {
            saw_channel_disabled = true;
        }
        if (line.find("no in-process adapter") != std::string::npos) {
            saw_unimplemented = true;
        }
    }
    CHECK(saw_channel_disabled);
    CHECK(saw_unimplemented);

    // 渠道开、账号关。
    wiring.reset();
    config.channels["feishu"].enabled = true;
    config.channels["feishu"].accounts["work"].enabled = false;
    wiring = ChannelGatewayWiring::Create(MakeOptions(&config, root));
    REQUIRE(wiring != nullptr);
    CHECK(wiring->adapter_count() == 0);
    bool saw_account_disabled = false;
    for (const std::string& line : wiring->skipped()) {
        if (line.find("account_disabled") != std::string::npos) {
            saw_account_disabled = true;
        }
    }
    CHECK(saw_account_disabled);

    // 账号开、凭据缺失(三种来源都没配)。
    wiring.reset();
    config.channels["feishu"].accounts["work"].enabled = true;
    config.channels["feishu"].accounts["work"].secret.reset();
    config.channels["feishu"].accounts["work"].secret_env.reset();
    wiring = ChannelGatewayWiring::Create(MakeOptions(&config, root));
    REQUIRE(wiring != nullptr);
    CHECK(wiring->adapter_count() == 0);
    bool saw_credentials = false;
    for (const std::string& line : wiring->skipped()) {
        if (line.find("credentials_missing") != std::string::npos) {
            saw_credentials = true;
        }
    }
    CHECK(saw_credentials);
}

TEST_CASE("feishu_wiring: 混配渠道——qq 与 feishu 各自装配,互不干扰") {
    const auto root = MakeTempRoot("mixed");
    config::Config config;
    channel::ChannelUserConfig feishu;
    feishu.enabled = true;
    channel::ChannelAccountUserConfig feishu_account =
        channel::MakeFeishuTemplateAccount();
    feishu_account.enabled = true;
    feishu_account.app_id = "cli_app1";
    feishu_account.secret = std::string("inline-secret");
    feishu.accounts["work"] = feishu_account;
    config.channels["feishu"] = feishu;

    channel::ChannelUserConfig qq;
    qq.enabled = true;
    channel::ChannelAccountUserConfig qq_account = channel::MakeQqTemplateAccount();
    qq_account.enabled = true;
    qq_account.app_id = "APP1";
    qq_account.secret = std::string("inline-secret");
    qq.accounts["main"] = qq_account;
    config.channels["qqbot"] = qq;

    auto wiring = ChannelGatewayWiring::Create(MakeOptions(&config, root));
    REQUIRE(wiring != nullptr);
    CHECK(wiring->skipped().empty());
    CHECK(wiring->adapter_count() == 2);
    CHECK(wiring->menu_publisher_count() == 0);  // feishu 无菜单发布器
    // 推泵到两只账号都 Running(适配器 start → spool 目录落位)。
    const auto deadline = platform::WallClockNowMs() + 8'000;
    while (platform::WallClockNowMs() < deadline) {
        (void)wiring->TickOnce(platform::WallClockNowMs());
        bool all_running = true;
        for (const auto& snapshot : wiring->manager()->Snapshots()) {
            if (snapshot.state != channel::ChannelAccountState::Running) {
                all_running = false;
            }
        }
        if (all_running && !wiring->manager()->Snapshots().empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // 状态根各落各:<root>/feishu/work 与 <root>/qqbot/main。
    CHECK(std::filesystem::exists(root / "feishu" / "work"));
    CHECK(std::filesystem::exists(root / "qqbot" / "main"));
    CHECK(wiring->Close(5'000));
}

TEST_CASE("feishu_wiring: setup 提交——feishu 账号走模板骨架与受管密钥") {
    const auto root = MakeTempRoot("setup_commit");
    channel::ChannelConfigService::Options options;
    options.config_file = (root / "config.json").string();
    options.secrets_root = root / "secrets";

    channel::ChannelSetupCommitRequest request;
    request.channel_id = "feishu";
    request.account_id = "work";
    request.app_id = std::string("cli_setup_1");
    request.new_secret = std::string("SECRET-SETUP-1");
    request.dry_run = true;
    const auto preview = channel::ChannelConfigService::Commit(options, request);
    REQUIRE(preview.has_value());
    CHECK(preview->created_account);
    bool saw_template_note = false;
    for (const std::string& change : preview->changes) {
        if (change.find("飞书 模板") != std::string::npos) {
            saw_template_note = true;
        }
        // 预览不含密钥值。
        CHECK(change.find("SECRET-SETUP-1") == std::string::npos);
    }
    CHECK(saw_template_note);
    // dry_run 一页未写。
    CHECK_FALSE(std::filesystem::exists(root / "config.json"));

    request.dry_run = false;
    const auto committed = channel::ChannelConfigService::Commit(options, request);
    REQUIRE(committed.has_value());
    // 写入的账号:模板五可选项 + secret_file(受管)接管密钥来源。
    const auto written = nlohmann::json::parse(
        [&] {
            std::ifstream input(root / "config.json", std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>());
        }(),
        nullptr, /*allow_exceptions=*/false);
    REQUIRE_FALSE(written.is_discarded());
    REQUIRE(written.contains("channels"));
    REQUIRE(written["channels"].contains("feishu"));
    const nlohmann::json& account = written["channels"]["feishu"]["accounts"]["work"];
    CHECK(account.at("app_id") == "cli_setup_1");
    CHECK(account.at("transport") == "websocket");
    CHECK(account.at("dm_policy") == "pairing");
    CHECK(account.at("group_policy") == "disabled");
    CHECK(account.at("reply").at("mode") == "final");
    CHECK(account.contains("secret_file"));       // 受管文件接管
    CHECK_FALSE(account.contains("secret_env"));  // 模板预指的 env 引用被清掉
    // 配置 JSON 里没有密钥值。
    const std::string config_text = [&] {
        std::ifstream input(root / "config.json", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>());
    }();
    CHECK(config_text.find("SECRET-SETUP-1") == std::string::npos);
}

TEST_CASE("feishu_wiring: setup 守门——未知平台的认得清单从平台表拼") {
    channel::ChannelSetupCommitRequest request;
    request.channel_id = "nonexistent";
    request.account_id = "main";
    request.app_id = std::string("x");
    channel::ChannelConfigService::Options options;
    options.config_file = "/dev/null/nonexistent-config.json";
    options.secrets_root = "/dev/null/nonexistent-secrets";
    const auto rejected = channel::ChannelConfigService::Commit(options, request);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().reason == "setup_bad_platform");
    // 认得清单:两平台都列(可配置),不再手写"认得: qqbot"。
    CHECK(rejected.error().detail.find("qqbot") != std::string::npos);
    CHECK(rejected.error().detail.find("feishu") != std::string::npos);
}

}  // namespace lubancode::app
