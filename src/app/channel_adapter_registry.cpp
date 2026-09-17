// 渠道适配器注册表的注册行(见 hpp 合同注释)。qq 注册进表;新渠道各
// 注册一行即接入装配主循环。
#include "app/channel_adapter_registry.hpp"

#include <utility>

#include "channel/qq/qq_media.hpp"

namespace lubancode::app {

namespace {

// qq 注册行:QqBotAdapter 装配 + Q7 菜单/面板发布器(显式启用才有账)。
// 原 channel_gateway_wiring.cpp 内联装配段原样搬来(R0 注册表化,零行为
// 变化)。
std::optional<ChannelAccountAssemblyResult> AssembleQqAccount(
    const ChannelAssemblyDeps& deps, const ChannelAccountAssembly& account,
    std::string* /*error*/) {
    channel::qq::QqBotAdapter::Options adapter_options;
    adapter_options.channel_id = account.channel_id;
    adapter_options.account_id = account.account_id;
    adapter_options.config = account.config;
    adapter_options.credential = account.credential;
    adapter_options.state_root = account.channels_state_root;
    adapter_options.http = deps.qq_http;
    adapter_options.transport_factory = deps.qq_transport_factory;
    adapter_options.ca_pem = deps.qq_ca_pem;
    adapter_options.trust_load_block_code = deps.qq_trust_load_block_code;
    adapter_options.trust_load_block_detail = deps.qq_trust_load_block_detail;
    adapter_options.now_ms = account.now_ms;
    auto adapter = std::make_unique<channel::qq::QqBotAdapter>(std::move(adapter_options));
    auto* adapter_ptr = adapter.get();

    ChannelAccountAssemblyResult result;
    // 连接状态取数口(reporter 每 tick 调;适配器所有权归 wiring 的
    // adapters_,指针活过账号生命周期)。
    result.connection_state = [adapter_ptr]() { return adapter_ptr->ConnectionState(); };
    // Q7 菜单/面板发布:显式启用(menu.publish 或 panel.enabled)才挂
    // 发布器;不启用的账号零行为变化(不碰平台菜单/面板)。
    if (account.config.menu.has_value() &&
        (account.config.menu->publish ||
         (account.config.menu->panel.has_value() && account.config.menu->panel->enabled))) {
        channel::qq::QqMenuPanelPublisher::Options publisher_options;
        publisher_options.http = deps.qq_http;
        publisher_options.tokens = adapter_ptr->token_manager();
        publisher_options.state_file = account.channels_state_root / account.channel_id /
                                       account.account_id / "menu-panel.json";
        publisher_options.now_ms = account.now_ms;
        result.menu_publisher =
            std::make_unique<channel::qq::QqMenuPanelPublisher>(std::move(publisher_options));
    }
    result.adapter = std::move(adapter);
    return result;
}

}  // namespace

const std::map<std::string, ChannelAdapterRegistration>& ChannelAdapterRegistry() {
    static const std::map<std::string, ChannelAdapterRegistration> registry = {
        // qq(Q1 定案进程内直连,§十五):唯一已实现渠道。媒体下载 seam
        // 与 turn 工作线程数也在这里注册(gateway_launch 按注册渠道取,
        // 不再内联绑 qq 实现)。
        {"qqbot",
         ChannelAdapterRegistration{
             AssembleQqAccount,
             // Q4 媒体接纳 seam:QQ 定案进程内直连——下载直接绑 qq 实现
             //(url 安全校验/大小帽/凭据脱敏都在里面;§十 10.1)。原
             // gateway_launch 内联装配段原样搬来。
             [] {
                 constexpr std::int64_t kMediaCapBytes =
                     20 * 1024 * 1024;  // 官方/插件/示例三口径取最小
                 const auto media_http = channel::qq::MakeMediaHttpFunc(
                     /*hard_timeout_ms=*/60'000, kMediaCapBytes);
                 return runtime::ChannelMediaDownloadFn(
                     [media_http](const std::string& url)
                         -> std::expected<runtime::ChannelMediaBytes, std::string> {
                     channel::qq::QqMediaDownloadLimits limits;
                     limits.max_bytes = kMediaCapBytes;
                     const auto downloaded =
                         channel::qq::DownloadQqAttachment(media_http, url, limits);
                     if (!downloaded.has_value()) {
                         // 稳定码 + 脱敏 detail(渠道实现保证 query 不进文案)。
                         return std::unexpected(downloaded.error().code + ": " +
                                                downloaded.error().detail);
                     }
                     return runtime::ChannelMediaBytes{std::move(downloaded->bytes)};
                 });
             }(),
             1,
         }},
    };
    return registry;
}

const std::vector<std::string>& ImplementedChannelAdapterIds() {
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> out;
        for (const auto& [channel_id, registration] : ChannelAdapterRegistry()) {
            (void)registration;
            out.push_back(channel_id);
        }
        return out;
    }();
    return ids;
}

}  // namespace lubancode::app
