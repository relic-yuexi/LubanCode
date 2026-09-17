// 企微装配接线册(W1):注册表挂行、setup 平台表与模板、wiring 真装配、
// im 文案认得清单。网关传输注假(自动应答订阅,静默读),零外联。
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/channel_adapter_registry.hpp"
#include "app/channel_gateway_wiring.hpp"
#include "app/im_entry.hpp"
#include "channel/channel_setup.hpp"
#include "channel/wecombot/wecom_gateway.hpp"
#include "config/config.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::app {
namespace {

// 假网关传输:Connect 插一发订阅回执(errcode=0),其余静默(读恒超时,
// 网关线程在线安静转)。
class QuietWecomTransport final : public channel::wecombot::WecomTransport {
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
                     ("lubancode-wecom-wiring-test-" + std::string(tag));
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
    options.test_transport_factory = []() {
        return std::unique_ptr<channel::qq::IGatewayTransport>(
            std::make_unique<QuietWecomTransport>());
    };
    return options;
}

config::Config MakeWecomConfig() {
    config::Config config;
    channel::ChannelUserConfig wecom;
    wecom.enabled = true;
    channel::ChannelAccountUserConfig account = channel::MakeWecombotTemplateAccount();
    account.enabled = true;
    account.app_id = "BOT1";  // app_id 字段存 BotID
    // 模板带 secret_env(WECOMBOT_SECRET);测试环境没设这个变量,env 路会
    // 判 credentials_missing——清掉走 inline 明文(resolver 三级同规矩)。
    account.secret_env.reset();
    account.secret = std::string("inline-secret");
    wecom.accounts["main"] = account;
    config.channels["wecombot"] = wecom;
    return config;
}

}  // namespace

// ---------------------------------------------------------------------------
// 注册表与文案
// ---------------------------------------------------------------------------

TEST_CASE("wecom_wiring: 注册表挂 wecombot 行(turn 线程 1,无媒体 seam)") {
    const auto& registry = ChannelAdapterRegistry();
    const auto it = registry.find("wecombot");
    REQUIRE(it != registry.end());
    CHECK(it->second.assemble_account != nullptr);
    CHECK(it->second.channel_turn_workers == 1);
    CHECK_FALSE(it->second.media_download);

    const auto& ids = ImplementedChannelAdapterIds();
    bool saw_wecom = false;
    bool saw_qq = false;
    for (const std::string& id : ids) {
        saw_wecom = saw_wecom || id == "wecombot";
        saw_qq = saw_qq || id == "qqbot";
    }
    CHECK(saw_wecom);
    CHECK(saw_qq);  // 既有渠道不受牵连
}

TEST_CASE("wecom_wiring: setup 平台表与模板——BotID/Secret 语义落账") {
    const auto& platforms = channel::ChannelSetupPlatforms();
    const auto platform = channel::FindChannelSetupPlatform("wecombot");
    REQUIRE(platform.has_value());
    CHECK(platform->implemented);
    CHECK(platform->display_name == "企业微信智能机器人");
    REQUIRE(platform->fields.size() == 2);
    CHECK(platform->fields[0].id == "app_id");
    CHECK(platform->fields[0].label == "BotID");
    CHECK_FALSE(platform->fields[0].sensitive);
    CHECK(platform->fields[1].id == "app_secret");
    CHECK(platform->fields[1].label == "Secret");
    CHECK(platform->fields[1].sensitive);

    const channel::ChannelAccountUserConfig template_account =
        channel::MakeWecombotTemplateAccount();
    CHECK(template_account.transport == "websocket");
    CHECK(template_account.secret_env.has_value());
    CHECK(*template_account.secret_env == "WECOMBOT_SECRET");
    CHECK(template_account.dm_policy == channel::DmPolicy::Pairing);
    CHECK(template_account.group_policy == channel::GroupPolicy::Disabled);
    CHECK_FALSE(template_account.allow_bots);
    CHECK(template_account.require_mention);
    CHECK(template_account.reply.mode == channel::ReplyMode::Final);
    REQUIRE(template_account.tools.allow.has_value());
    CHECK(template_account.tools.allow->size() >= 2);
}

TEST_CASE("wecom_wiring: setup 提交——wecombot 走自家模板(dry_run 差异)") {
    const auto root = MakeTempRoot("setup_dry");
    channel::ChannelConfigService::Options options;
    options.config_file = (root / "config.json").string();
    options.secrets_root = (root / "secrets").string();
    options.lock_timeout_ms = 2'000;
    channel::ChannelSetupCommitRequest request;
    request.channel_id = "wecombot";
    request.account_id = "main";
    request.app_id = "BOT-123";
    request.new_secret = std::string("WECOMBOT-SECRET-for-test");
    request.dry_run = true;
    const auto committed = channel::ChannelConfigService::Commit(options, request);
    REQUIRE(committed.has_value());
    CHECK(committed->created_account);
    bool saw_template_note = false;
    for (const std::string& change : committed->changes) {
        if (change.find("企业微信智能机器人 模板") != std::string::npos) {
            saw_template_note = true;
        }
        CHECK(change.find("QQ 模板") == std::string::npos);  // 不串门
    }
    CHECK(saw_template_note);
}

TEST_CASE("wecom_wiring: setup 未知平台报错文案含 wecombot(单一真源)") {
    channel::ChannelConfigService::Options options;
    options.config_file = (std::filesystem::temp_directory_path() / "no-such-config.json").string();
    options.secrets_root = (std::filesystem::temp_directory_path() / "no-such-secrets").string();
    channel::ChannelSetupCommitRequest request;
    request.channel_id = "ghostim";
    request.account_id = "main";
    request.app_id = "X";
    const auto committed = channel::ChannelConfigService::Commit(options, request);
    REQUIRE_FALSE(committed.has_value());
    CHECK(committed.error().reason == "setup_bad_platform");
    CHECK(committed.error().detail.find("wecombot") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 装配与起跑
// ---------------------------------------------------------------------------

TEST_CASE("wecom_wiring: 五闸全过——wecombot 账号真装配进 ChannelManager 并起跑") {
    const auto root = MakeTempRoot("ready");
    config::Config config = MakeWecomConfig();

    auto wiring = ChannelGatewayWiring::Create(MakeOptions(&config, root));
    REQUIRE(wiring != nullptr);
    CHECK(wiring->skipped().empty());
    CHECK(wiring->adapter_count() == 1);
    REQUIRE(wiring->manager() != nullptr);
    const auto snapshots = wiring->manager()->Snapshots();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].channel_id == "wecombot");
    CHECK(snapshots[0].account_id == "main");

    // TickOnce 推进桥泵直到 Running(initialize/start 握手照走;网关在线
    // 与否是 Health 的细账,不拦 manager 状态)。
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

TEST_CASE("wecom_wiring: 未注册渠道照旧 skipped;wecombot 与 qqbot 并存不串") {
    const auto root = MakeTempRoot("mixed");
    config::Config config = MakeWecomConfig();
    // 再放一只 QQ 账号与一只未实现渠道。
    channel::ChannelUserConfig qq;
    qq.enabled = true;
    channel::ChannelAccountUserConfig qq_account = channel::MakeQqTemplateAccount();
    qq_account.enabled = true;
    qq_account.app_id = "APP1";
    qq_account.secret = std::string("inline-secret");
    qq.accounts["main"] = qq_account;
    config.channels["qqbot"] = qq;
    config.channels["fakeim"] = channel::ChannelUserConfig{};

    auto wiring = ChannelGatewayWiring::Create(MakeOptions(&config, root));
    REQUIRE(wiring != nullptr);
    CHECK(wiring->adapter_count() == 2);
    bool saw_unimplemented = false;
    for (const std::string& line : wiring->skipped()) {
        if (line.find("no in-process adapter") != std::string::npos) {
            saw_unimplemented = true;
        }
    }
    CHECK(saw_unimplemented);
    CHECK(wiring->Close(5'000));
}

TEST_CASE("wecom_wiring: im 认得清单——wecombot 可选,拼错的报错带上它") {
    config::Config config = MakeWecomConfig();
    ImTargetQuery query;
    query.explicit_channel = "wecombot";
    query.channels = &config.channels;
    const auto resolution = ResolveImTarget(query);
    CHECK(resolution.status == ImTargetResolution::Status::ReadyUnique);
    REQUIRE(resolution.target.has_value());
    CHECK(resolution.target->channel_id == "wecombot");
    CHECK(resolution.target->account_id == "main");

    ImTargetQuery unknown;
    unknown.explicit_channel = "ghostim";
    const auto unknown_resolution = ResolveImTarget(unknown);
    CHECK(unknown_resolution.status == ImTargetResolution::Status::UnknownPlatform);
    CHECK(unknown_resolution.detail.find("wecombot") != std::string::npos);
}

}  // namespace lubancode::app
