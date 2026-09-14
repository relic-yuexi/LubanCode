// 渠道装配实现(见 hpp 合同注释)。
#include "app/channel_gateway_wiring.hpp"

#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

#include "channel/activation.hpp"
#include "channel/credentials.hpp"
#include "channel/qq/qq_adapter.hpp"
#include "channel/qq/qq_gateway.hpp"
#include "channel/qq/qq_http.hpp"
#include "channel/qq/qq_tls.hpp"
#include "config/config.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::app {

namespace {

// Q1 只实现 qqbot 的进程内适配器;其余渠道名如实记 skipped。
constexpr const char* kImplementedChannelId = "qqbot";

std::string ReadFileToString(const std::filesystem::path& path) {
    std::FILE* file = nullptr;
#ifdef _WIN32
    file = _wfopen(path.c_str(), L"rb");
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    if (file == nullptr) {
        return std::string();
    }
    std::string content;
    char buffer[8192];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        content.append(buffer, got);
    }
    std::fclose(file);
    return content;
}

}  // namespace

// ---------------------------------------------------------------------------
// CompositeGatewayPump
// ---------------------------------------------------------------------------

CompositeGatewayPump::CompositeGatewayPump(
    gateway::GatewayWorkPump* primary,
    std::unique_ptr<gateway::GatewayWorkPump> secondary)
    : primary_(primary), secondary_(std::move(secondary)) {}

CompositeGatewayPump::~CompositeGatewayPump() = default;

bool CompositeGatewayPump::TickOnce(std::int64_t now_ms) {
    bool ok = true;
    if (primary_ != nullptr && !primary_->TickOnce(now_ms)) {
        ok = false;
    }
    if (secondary_ != nullptr && !secondary_->TickOnce(now_ms)) {
        ok = false;
    }
    return ok;
}

void CompositeGatewayPump::StopAccepting() {
    if (primary_ != nullptr) {
        primary_->StopAccepting();
    }
    if (secondary_ != nullptr) {
        secondary_->StopAccepting();
    }
}

void CompositeGatewayPump::set_owner_epoch(const std::string& epoch) {
    if (primary_ != nullptr) {
        primary_->set_owner_epoch(epoch);
    }
    if (secondary_ != nullptr) {
        secondary_->set_owner_epoch(epoch);
    }
}

bool CompositeGatewayPump::Close(int grace_ms) {
    bool ok = true;
    if (primary_ != nullptr && !primary_->Close(grace_ms)) {
        ok = false;
    }
    if (secondary_ != nullptr && !secondary_->Close(grace_ms)) {
        ok = false;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// ChannelGatewayWiring
// ---------------------------------------------------------------------------

std::unique_ptr<ChannelGatewayWiring> ChannelGatewayWiring::Create(Options options) {
    if (options.config == nullptr || options.config->channels.empty()) {
        return nullptr;  // 零渠道行为:不挂泵,不起线程
    }

    auto wiring = std::unique_ptr<ChannelGatewayWiring>(new ChannelGatewayWiring());

    channel::ChannelManagerOptions manager_options;
    manager_options.state_root = options.channels_state_root;
    manager_options.now_ms = options.now_ms;
    wiring->manager_ = std::make_unique<channel::ChannelManager>(std::move(manager_options));

    // QQ 定案进程内直连(§十五):渠道实现内置受信,不造假包概念。
    const channel::ChannelTrustState builtin_trust{/*installed=*/true, /*trusted=*/true};

    std::string ca_pem = options.ca_pem;
    if (ca_pem.empty()) {
        const std::string detected = channel::qq::DetectSystemCaPemPath();
        if (!detected.empty()) {
            ca_pem = ReadFileToString(std::filesystem::path(detected));
        }
    }

    const auto now_ms = options.now_ms ? options.now_ms
                                        : static_cast<std::int64_t (*)()>(
                                              &platform::WallClockNowMs);
    const auto http = options.test_http ? options.test_http
                                        : channel::qq::MakeDefaultHttpFunc();
    const auto transport_factory =
        options.test_transport_factory
            ? std::move(options.test_transport_factory)
            : channel::qq::MakeWsTransportFactory(ca_pem);

    for (const auto& [channel_id, channel_config] : options.config->channels) {
        if (channel_id != kImplementedChannelId) {
            wiring->skipped_.push_back("channel " + channel_id +
                                       ": no in-process adapter implemented (Q1)");
            continue;
        }
        for (const auto& [account_id, account_config] : channel_config.accounts) {
            channel::ChannelCredentialState credential_state;
            const auto credential = channel::ResolveChannelCredential(account_config);
            if (credential.has_value()) {
                credential_state.ready = true;
            } else {
                credential_state.reason = credential.error().reason;
            }
            const channel::ChannelLockState lock_unused{};  // 锁权威在 AddAccount
            const auto decision = channel::ResolveChannelActivation(
                channel::ChannelProcessMode::Gateway, builtin_trust,
                std::optional<channel::ChannelUserConfig>(channel_config), channel_id,
                account_id, credential_state, lock_unused);
            if (!decision.ready()) {
                wiring->skipped_.push_back(channel_id + "/" + account_id + ": " +
                                           channel::ChannelActivationCodeName(decision.code) +
                                           " (" + decision.detail + ")");
                continue;
            }
            // 凭据 ready:真值进程内持有,不落日志。
            channel::qq::QqBotAdapter::Options adapter_options;
            adapter_options.channel_id = channel_id;
            adapter_options.account_id = account_id;
            adapter_options.config = account_config;
            adapter_options.credential = *credential;
            adapter_options.state_root = options.channels_state_root;
            adapter_options.http = http;
            adapter_options.transport_factory = transport_factory;
            adapter_options.ca_pem = ca_pem;
            adapter_options.now_ms = now_ms;
            auto adapter = std::make_unique<channel::qq::QqBotAdapter>(
                std::move(adapter_options));
            auto* adapter_ptr = adapter.get();
            const auto added = wiring->manager_->AddAccount(channel_id, account_id,
                                                            account_config, adapter_ptr);
            if (added.status != channel::ChannelManager::AddAccountResult::Status::Ok) {
                wiring->skipped_.push_back(channel_id + "/" + account_id + ": add_failed (" +
                                           added.detail + ")");
                continue;  // adapter 析构收线程
            }
            wiring->adapters_.push_back(std::move(adapter));
        }
        // 渠道层 bindings/tools(Q0 五层交集的渠道层)。
        wiring->manager_->SetChannelBindings(channel_id, channel_config.bindings);
        wiring->manager_->SetChannelToolsPolicy(channel_id, channel_config.tools);
    }

    // 装配过的账号统一起跑(AddAccount 只入账;StartAccount 发 initialize/
    // start,由 TickOnce 的 Pump 推进握手)。
    for (const auto& snapshot : wiring->manager_->Snapshots()) {
        (void)wiring->manager_->StartAccount(snapshot.channel_id, snapshot.account_id);
    }
    return wiring;
}

ChannelGatewayWiring::~ChannelGatewayWiring() {
    if (manager_ == nullptr) {
        return;
    }
    // 析构兜底:停账号(幂等;ChannelManager 析构走同一条路)。
    for (const auto& snapshot : manager_->Snapshots()) {
        (void)manager_->StopAccount(snapshot.channel_id, snapshot.account_id);
    }
    PumpAll();
}

void ChannelGatewayWiring::PumpAll() {
    if (manager_ == nullptr) {
        return;
    }
    for (const auto& snapshot : manager_->Snapshots()) {
        manager_->Pump(snapshot.channel_id, snapshot.account_id);
    }
}

bool ChannelGatewayWiring::TickOnce(std::int64_t now_ms) {
    (void)now_ms;
    if (manager_ == nullptr) {
        return true;
    }
    PumpAll();
    if (work_pump_ != nullptr && !work_pump_->TickOnce(now_ms)) {
        return false;  // 渠道业务泵 broken(账写不进):停业务 tick
    }
    return true;
}

void ChannelGatewayWiring::StopAccepting() {
    if (work_pump_ != nullptr) {
        work_pump_->StopAccepting();
    }
}

void ChannelGatewayWiring::set_owner_epoch(const std::string& epoch) {
    if (work_pump_ != nullptr) {
        work_pump_->set_owner_epoch(epoch);
    }
}

void ChannelGatewayWiring::set_work_pump(std::unique_ptr<gateway::GatewayWorkPump> work_pump) {
    work_pump_ = std::move(work_pump);
}

bool ChannelGatewayWiring::Close(int grace_ms) {
    // 渠道 work 泵先收口(渠道活场封口;不再碰 manager)。
    bool ok = true;
    if (work_pump_ != nullptr && !work_pump_->Close(grace_ms)) {
        ok = false;
    }
    work_pump_.reset();
    if (manager_ == nullptr) {
        return ok;
    }
    for (const auto& snapshot : manager_->Snapshots()) {
        (void)manager_->StopAccount(snapshot.channel_id, snapshot.account_id);
    }
    const auto deadline = platform::WallClockNowMs() + grace_ms;
    while (platform::WallClockNowMs() < deadline) {
        PumpAll();
        bool all_stopped = true;
        for (const auto& snapshot : manager_->Snapshots()) {
            if (snapshot.state != channel::ChannelAccountState::Stopped) {
                all_stopped = false;
            }
        }
        if (all_stopped) {
            return ok;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

}  // namespace lubancode::app
