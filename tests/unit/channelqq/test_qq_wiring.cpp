// 渠道装配接线册(QQ 机器人接入单 Q1,Q0 留件"激活函数接线与 trust 快照"
// 的验收):ChannelGatewayWiring::Create 对每只账号跑权威激活函数——分支
// 裁决(disabled/凭据缺失/渠道未实现)、Ready 账号真装配(QqBotAdapter 进
// ChannelManager,StartAccount 起网关线程)。网关传输与 HTTP 全注假,零外联。
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/channel_gateway_wiring.hpp"
#include "channel/qq/qq_gateway.hpp"
#include "config/config.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::app {
namespace {

// 静默假网关传输:连接即过,不推事件,读恒超时(网关线程在退避循环里安静转)。
class QuietGatewayTransport final : public channel::qq::IGatewayTransport {
public:
    std::expected<void, std::string> Connect(const std::string&) override { return {}; }
    std::expected<void, std::string> SendText(const std::string&) override { return {}; }
    std::expected<std::string, channel::qq::WsError> ReadMessage(int) override {
        return std::unexpected(channel::qq::WsError{channel::qq::WsError::Kind::Timeout,
                                                    "quiet", 0});
    }
    void Cancel() override {}
    void Close(std::uint16_t, const std::string&) override {}
};

channel::qq::QqHttpFunc QuietHttp() {
    return [](const channel::qq::QqHttpRequest&)
        -> std::expected<channel::qq::QqHttpResponse, std::string> {
        return std::unexpected("quiet test http");
    };
}

std::filesystem::path MakeTempRoot(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-qq-wiring-test-" + std::string(tag));
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
            std::make_unique<QuietGatewayTransport>());
    };
    options.test_http = QuietHttp();
    return options;
}

}  // namespace

TEST_CASE("qq_wiring: 无 channels 配置零装配(不起泵)") {
    const auto root = MakeTempRoot("empty");
    config::Config config;
    auto wiring = ChannelGatewayWiring::Create(MakeOptions(&config, root));
    CHECK(wiring == nullptr);
}

TEST_CASE("qq_wiring: 分支裁决——渠道禁用/账号禁用/凭据缺失/未实现渠道") {
    const auto root = MakeTempRoot("branches");
    config::Config config;
    channel::ChannelUserConfig qq;
    qq.enabled = false;  // 渠道禁用
    channel::ChannelAccountUserConfig account = channel::MakeQqTemplateAccount();
    account.enabled = true;
    account.secret = std::string("inline-secret");
    qq.accounts["main"] = account;
    config.channels["qqbot"] = qq;
    // 未实现渠道。
    config.channels["fakeim"] = channel::ChannelUserConfig{};

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
    config.channels["qqbot"].enabled = true;
    config.channels["qqbot"].accounts["main"].enabled = false;
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
    config.channels["qqbot"].accounts["main"].enabled = true;
    config.channels["qqbot"].accounts["main"].secret.reset();
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

TEST_CASE("qq_wiring: 五闸全过——账号真装配进 ChannelManager 并起跑") {
    const auto root = MakeTempRoot("ready");
    config::Config config;
    channel::ChannelUserConfig qq;
    qq.enabled = true;
    channel::ChannelAccountUserConfig account = channel::MakeQqTemplateAccount();
    account.enabled = true;
    account.app_id = "APP1";
    account.secret = std::string("inline-secret");
    qq.accounts["main"] = account;
    config.channels["qqbot"] = qq;

    auto wiring = ChannelGatewayWiring::Create(MakeOptions(&config, root));
    REQUIRE(wiring != nullptr);
    CHECK(wiring->skipped().empty());
    CHECK(wiring->adapter_count() == 1);
    REQUIRE(wiring->manager() != nullptr);
    const auto snapshots = wiring->manager()->Snapshots();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].channel_id == "qqbot");
    CHECK(snapshots[0].account_id == "main");
    // 装配后 StartAccount 已发;TickOnce 推进桥泵直到 Running(假网关静默
    // 但 initialize/start 握手照走)。
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

    // 关机次序:Close 停账号收线程(宽限内)。
    CHECK(wiring->Close(5'000));
    wiring.reset();
}

}  // namespace lubancode::app
