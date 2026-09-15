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
#include "gateway/pairing_command.hpp"
#include "platform/process.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::app {

namespace {

// Q1 只实现 qqbot 的进程内适配器;其余渠道名如实记 skipped。
constexpr const char* kImplementedChannelId = "qqbot";

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
    wiring->control_dir_ = options.gateway_control_dir;

    // QQ 定案进程内直连(§十五):渠道实现内置受信,不造假包概念。
    const channel::ChannelTrustState builtin_trust{/*installed=*/true, /*trusted=*/true};

    // 信任根解析(§四):显式 ca_pem(测试位/覆盖位)优先且不回退;空则按
    // 平台取默认(Windows 系统证书库;Linux/macOS 系统 PEM)。解析不到/
    // 失败明报进 diagnostics——不静默放行,也不拦装配(连接时按
    // tls_trust_store_empty 稳定码失败,现场能定位第一处失败)。
    const channel::qq::ResolvedTrustStore trust =
        channel::qq::ResolveChannelTrustRoots(options.ca_pem);
    const channel::qq::TlsTrustMode trust_mode = options.ca_pem.empty()
                                                     ? channel::qq::TlsTrustMode::SystemDefault
                                                     : channel::qq::TlsTrustMode::ExplicitCa;
    const std::string& ca_pem = trust.ca_pem;
    if (!trust.error.empty()) {
        wiring->diagnostics_.push_back("TLS 信任根不可用(" + trust.error +
                                       ")——QQ 连接将失败(tls_trust_store_empty)");
    } else {
        wiring->diagnostics_.push_back("TLS 信任根:" + trust.detail + "(" +
                                       std::to_string(trust.certificate_count) + " 张)");
    }

    const auto now_ms = options.now_ms ? options.now_ms
                                        : static_cast<std::int64_t (*)()>(
                                              &platform::WallClockNowMs);
    const auto http = options.test_http ? options.test_http
                                        : channel::qq::MakeDefaultHttpFunc();
    const auto transport_factory =
        options.test_transport_factory
            ? std::move(options.test_transport_factory)
            : channel::qq::MakeWsTransportFactory(ca_pem, trust_mode);

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
            wiring->adapter_views_.push_back(
                AdapterView{channel_id, account_id,
                            static_cast<channel::qq::QqBotAdapter*>(adapter_ptr)});
            // Q7 菜单/面板发布:显式启用(menu.publish 或 panel.enabled)才
            // 挂发布器;不启用的账号零行为变化(不碰平台菜单/面板)。
            if (account_config.menu.has_value() &&
                (account_config.menu->publish ||
                 (account_config.menu->panel.has_value() && account_config.menu->panel->enabled))) {
                channel::qq::QqMenuPanelPublisher::Options publisher_options;
                publisher_options.http = http;
                publisher_options.tokens =
                    static_cast<channel::qq::QqBotAdapter*>(adapter_ptr)->token_manager();
                publisher_options.state_file = options.channels_state_root / channel_id /
                                               account_id / "menu-panel.json";
                publisher_options.now_ms = now_ms;
                MenuPublisherEntry entry;
                entry.channel_id = channel_id;
                entry.account_id = account_id;
                entry.menu = *account_config.menu;
                entry.publisher =
                    std::make_unique<channel::qq::QqMenuPanelPublisher>(std::move(publisher_options));
                wiring->menu_publishers_.push_back(std::move(entry));
            }
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

    // 连接状态宿主输出件(§三):每 tick 限频打印连接状态 + 发布跨进程
    // 只读快照(带 boot ID 与更新时间,CLI 校验存活与过期)。
    if (!wiring->adapter_views_.empty()) {
        ChannelConnectionReporter::Deps reporter_deps;
        reporter_deps.channels_state_root = options.channels_state_root;
        reporter_deps.emit = [](const std::string& line) {
            std::fprintf(stderr, "%s\n", line.c_str());
        };
        for (const AdapterView& view : wiring->adapter_views_) {
            ChannelConnectionReporter::Account reporter_account;
            reporter_account.channel_id = view.channel_id;
            reporter_account.account_id = view.account_id;
            reporter_account.snapshot = [adapter = view.adapter]() {
                return adapter->ConnectionState();
            };
            reporter_deps.accounts.push_back(std::move(reporter_account));
        }
        wiring->reporter_ =
            std::make_unique<ChannelConnectionReporter>(std::move(reporter_deps));
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
    if (manager_ == nullptr) {
        return true;
    }
    PumpAll();
    // 连接状态输出与快照发布(§三):boot_id 即 owner_epoch(Gateway 取锁
    // 后递进);pid 现取。账号失败不拦主业务(单账号失败不拖死其他)。
    if (reporter_ != nullptr) {
        reporter_->Observe(owner_epoch_, platform::CurrentProcessId(), now_ms);
    }
    // Q1b 配对控制面:另一终端的 approve/reject 命令在这里落地(回执文件
    // 由 CLI 读走;命令消费失败不拦主业务——下一条命令下拍再来)。
    if (!control_dir_.empty()) {
        ConsumePairingCommands();
    }
    // Q7 菜单/面板发布(显式启用才有账):首拍 + 配对增删后的重同步。
    // 发布失败不拦主业务(退避重试,单账号不拖死其他)。
    if (!menu_publishers_.empty()) {
        PumpMenuSync(now_ms);
    }
    if (work_pump_ != nullptr && !work_pump_->TickOnce(now_ms)) {
        return false;  // 渠道业务泵 broken(账写不进):停业务 tick
    }
    return true;
}

void ChannelGatewayWiring::PumpMenuSync(std::int64_t now_ms) {
    for (auto& entry : menu_publishers_) {
        if (!entry.pending && now_ms < entry.retry_at_ms) {
            continue;
        }
        channel::qq::QqMenuPanelPublisher::SyncInput input;
        input.channel_id = entry.channel_id;
        input.account_id = entry.account_id;
        input.publish_menu = entry.menu.publish;
        input.menu = entry.menu;
        input.publish_panel = entry.menu.panel.has_value() && entry.menu.panel->enabled;
        if (input.publish_panel) {
            input.panel = *entry.menu.panel;
            if (input.panel.target_type == "specific") {
                // c2c specific:关联对象 = 当前已配对 sender(撤销配对 → 重
                // 同步时按本地账移除;面板可见不是宿主授权,准入照旧走路由)。
                input.desired_targets =
                    manager_->ApprovedPairingSenders(entry.channel_id, entry.account_id);
            }
        }
        const auto report = entry.publisher->Sync(input);
        for (const auto& notice : report.notices) {
            std::fprintf(stderr, "[gateway] QQ 菜单 %s/%s: %s\n", entry.channel_id.c_str(),
                         entry.account_id.c_str(), notice.c_str());
        }
        if (!report.error_code.empty()) {
            std::fprintf(stderr, "[gateway] QQ 菜单 %s/%s 发布失败(%s):%s\n",
                         entry.channel_id.c_str(), entry.account_id.c_str(),
                         report.error_code.c_str(), report.error_detail.c_str());
            entry.retry_at_ms =
                report.retry_at_ms > now_ms ? report.retry_at_ms : now_ms + 60'000;
        } else {
            entry.retry_at_ms = 0;
        }
        entry.pending = false;
    }
}

void ChannelGatewayWiring::ConsumePairingCommands() {
    for (const gateway::GatewayPairingCommand& command :
         gateway::PollPairingCommands(control_dir_, owner_epoch_)) {
        gateway::GatewayPairingCommandResult result;
        result.command_id = command.command_id;
        result.action = command.action;
        // "配对码或身份"单参数口:先按 code 认(code 在提示正文里),认
        // 不出(not_found)再按 sender 身份认(身份在待审清单里)。
        std::string error;
        std::optional<std::string> sender;
        const bool approve = command.action == "approve";
        sender = approve ? manager_->ApprovePairing(command.channel_id, command.account_id,
                                                    command.token, &error)
                         : manager_->RejectPairing(command.channel_id, command.account_id,
                                                   command.token, &error);
        if (!sender.has_value() && error == "not_found") {
            sender = approve
                         ? manager_->ApprovePairingBySender(command.channel_id,
                                                            command.account_id, command.token,
                                                            &error)
                         : manager_->RejectPairingBySender(command.channel_id,
                                                           command.account_id, command.token,
                                                           &error);
        }
        result.ok = sender.has_value();
        result.sender_id = sender.value_or(std::string());
        result.error = error;
        if (result.ok) {
            result.detail = approve ? "已批准配对" : "已拒绝配对";
            std::fprintf(stderr, "[gateway] 渠道配对: %s %s/%s 身份 %s\n",
                         approve ? "已批准" : "已拒绝", command.channel_id.c_str(),
                         command.account_id.c_str(), result.sender_id.c_str());
            // Q7:c2c specific 面板的关联对象跟配对账走——增删后重同步
            //(下拍 PumpMenuSync 按 ApprovedPairingSenders 增量)。
            for (auto& entry : menu_publishers_) {
                if (entry.channel_id == command.channel_id &&
                    entry.account_id == command.account_id) {
                    entry.pending = true;
                }
            }
        } else if (error == "expired") {
            result.detail = "配对码已过期——让用户重新发一条消息领取新码";
        } else if (error == "already_finalized") {
            result.detail = "这笔配对已经处理过(一次性)";
        } else if (error == "not_found") {
            result.detail = "配对码与身份都不在待审账上——先让用户给机器人发条消息";
        } else if (error == "account_not_found") {
            result.detail = "这只渠道账号没在本 Gateway 装配(未启用或装配失败)";
        } else {
            result.detail = "配对账不可写: " + error;
        }
        const std::string write_error = gateway::WritePairingCommandResult(control_dir_, result);
        if (!write_error.empty()) {
            std::fprintf(stderr, "[gateway] 渠道配对回执写不进: %s\n", write_error.c_str());
        }
    }
}

void ChannelGatewayWiring::StopAccepting() {
    if (work_pump_ != nullptr) {
        work_pump_->StopAccepting();
    }
}

void ChannelGatewayWiring::set_owner_epoch(const std::string& epoch) {
    owner_epoch_ = epoch;
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
    bool all_stopped = false;
    while (platform::WallClockNowMs() < deadline) {
        PumpAll();
        all_stopped = true;
        for (const auto& snapshot : manager_->Snapshots()) {
            if (snapshot.state != channel::ChannelAccountState::Stopped) {
                all_stopped = false;
            }
        }
        if (all_stopped) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // 收口后把 stopped 状态刷进快照(CLI 侧不把停机前的旧快照当在线)。
    if (reporter_ != nullptr) {
        reporter_->Observe(owner_epoch_, platform::CurrentProcessId(),
                           platform::WallClockNowMs());
    }
    if (!all_stopped) {
        return false;
    }
    return ok;
}

}  // namespace lubancode::app
