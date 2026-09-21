// 进程内渠道适配器注册表(飞书/企微设计单 R0):channel_id → 账号级适配器
// 工厂。单一真源,三处消费:
//   - ChannelGatewayWiring::Create 的装配门:未注册渠道照旧记 skipped
//     ("no in-process adapter implemented"),语义与 Q1 单值门完全一致;
//   - im_entry 的"认得渠道"文案(ImplementedChannelAdapterIds);
//   - gateway_launch 的渠道件装配(媒体下载 seam / turn 工作线程数)。
// qq 注册进表;feishu/wecombot 各注册一行即接入——装配主循环渠道无关
// (五闸裁决/AddAccount/起跑/连接报告),渠道差异全收进注册行。
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "channel/channel_config.hpp"
#include "channel/connection_state.hpp"
#include "channel/credentials.hpp"
#include "channel/feishu/feishu_gateway.hpp"
#include "channel/feishu/feishu_http.hpp"
#include "channel/manager.hpp"
#include "channel/qq/qq_gateway.hpp"  // 迁移期:既有测试册仍拼 channel::qq::GatewayConnectError,经本头传递可见;SV-07 别名退役时一并删
#include "channel/qq/qq_http.hpp"
#include "channel/qq/qq_menu.hpp"
#include "channel/transport/gateway_transport.hpp"
#include "runtime/channel_media_service.hpp"

namespace lubancode::app {

// 账号级装配输入(主循环统一备好的渠道无关材料;五闸已过、凭据已解析)。
// 渠道私有的依赖(http/传输工厂/信任根)不进这里——走 ChannelAssemblyDeps,
// 新渠道带新依赖只加字段,不动别家注册行。
struct ChannelAccountAssembly {
    std::string channel_id;
    std::string account_id;
    channel::ChannelAccountUserConfig config;
    channel::ResolvedChannelCredential credential;  // 进程内持有,不外泄
    std::filesystem::path channels_state_root;  // 渠道账号状态根(适配器自拼 <ch>/<acct>)
    std::function<std::int64_t()> now_ms;
};

// 装配产物:适配器本体 + 连接状态取数口(reporter 用;快照是 channel
// 中立合同,三平台同一类型)+ QQ 菜单/面板发布器(Q7;渠道无此件则空)。
struct ChannelAccountAssemblyResult {
    std::unique_ptr<channel::ChannelBridgeTransport> adapter;
    std::function<channel::ConnectionSnapshot()> connection_state;
    std::unique_ptr<channel::qq::QqMenuPanelPublisher> menu_publisher;
};

// 注册行的装配材料(wiring 的 Create 每次现给):测试注入位与信任根解析
// 结果。字段归各渠道私有(qq_/feishu_ 前缀),注册行只取自家认得的;新
// 渠道带新依赖只加字段,不动别家注册行。
struct ChannelAssemblyDeps {
    channel::qq::QqHttpFunc qq_http;
    // WS 传输工厂(SV-07 起中立 seam;qq_ 前缀是命名史遗留——QQ/企微
    // 的网关传输同用这一件)。
    std::function<std::unique_ptr<channel::transport::IGatewayTransport>()>
        qq_transport_factory;
    std::string qq_ca_pem;
    std::string qq_trust_load_block_code;
    std::string qq_trust_load_block_detail;
    // 飞书(F1):HTTP seam(引导/令牌/回话同一路)与 WS 传输工厂。
    channel::feishu::FeishuHttpFunc feishu_http;
    std::function<std::unique_ptr<channel::feishu::IFeishuGatewayTransport>()>
        feishu_transport_factory;
    std::string feishu_ca_pem;
    std::string feishu_trust_load_block_code;
    std::string feishu_trust_load_block_detail;
};

// 渠道注册行。
struct ChannelAdapterRegistration {
    // 账号级装配(五闸已过、凭据已解析)。失败置 nullopt 并写 error
    // (主循环记 skipped,与五闸失败同一形态)。
    std::function<std::optional<ChannelAccountAssemblyResult>(
        const ChannelAssemblyDeps&, const ChannelAccountAssembly&, std::string* error)>
        assemble_account;
    // ---- 渠道件(gateway_launch 的 work 泵装配) ----
    // 媒体下载 seam(空 = 该渠道无媒体接纳,附件行如实报不可用,正文路
    // 照走)。
    runtime::ChannelMediaDownloadFn media_download;
    // 渠道 turn 工作线程数(0 = 不起专用线程;生产渠道应 ≥1,§12.2
    // 第九行:等按钮的线程不能是唯一收按钮线程)。
    std::size_t channel_turn_workers = 0;
};

// 注册表(键 = channel_id,静态单一真源;序即 map 序)。照
// ChannelSetupPlatforms 的函数口静态表风格。
const std::map<std::string, ChannelAdapterRegistration>& ChannelAdapterRegistry();

// 已注册渠道 id(im 文案等只读消费;与注册表同源)。
const std::vector<std::string>& ImplementedChannelAdapterIds();

}  // namespace lubancode::app
