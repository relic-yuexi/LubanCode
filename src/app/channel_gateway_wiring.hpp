// Gateway 的渠道装配与复合泵(QQ 机器人接入单 Q1,Q0 留件"激活接线与
// trust 快照装配"的落地)。
//
// 装配面(todo §五/Q0 记账):gateway run 启动路径读全局 channels 段,对每只
// 启用账号跑唯一权威激活函数 ResolveChannelActivation——五闸过了才把
// ChannelManager::AddAccount + StartAccount 真接上。QQ 定案进程内直连
// (§十五):trust 快照按"渠道实现内置受信"恒信任(不造假包概念),凭据经
// Q0 resolver 解析后留在宿主进程内,适配器是 ChannelBridgeTransport 的
// 进程内实现。已注册渠道(channel_adapter_registry,R0 起注册表化)查表
// 装配;未注册渠道如实记 skipped,不起任何线程。
//
// 复合泵:GatewayProcess::Options 只收一只泵,渠道的 Pump(出站帧 flush +
// 入站字节 drain)是有界同步件,与 automation 主泵同 tick——本件把两只
// GatewayWorkPump 叠一只,不动 engine 冻结面。
//
// 连接状态单 §三/§四:装配时解析 TLS 信任根(诊断行入 diagnostics,由
// cli 装配段打印——Windows 默认系统证书库,Linux/macOS 系统 PEM,显式
// ca_pem 为测试位/覆盖位);每 tick 推 ChannelConnectionReporter(宿主
// 输出连接状态 + 发布跨进程只读快照,boot ID 走 set_owner_epoch 递进)。
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/channel_connection_reporter.hpp"
#include "channel/connection_state.hpp"
#include "channel/feishu/feishu_gateway.hpp"
#include "channel/feishu/feishu_http.hpp"
#include "channel/manager.hpp"
#include "channel/qq/qq_http.hpp"
#include "channel/qq/qq_menu.hpp"
#include "channel/transport/gateway_transport.hpp"
#include "gateway/work_pump.hpp"

namespace lubancode::config {
struct Config;
}

namespace lubancode::app {

// 双泵叠加:TickOnce 先主后次,StopAccepting/Close/owner_epoch 两路转发。
// primary 是借用(装配层的栈对象,生命周期外层管);secondary 归本件所有。
class CompositeGatewayPump final : public gateway::GatewayWorkPump {
public:
    CompositeGatewayPump(gateway::GatewayWorkPump* primary,
                         std::unique_ptr<gateway::GatewayWorkPump> secondary);
    ~CompositeGatewayPump() override;

    bool TickOnce(std::int64_t now_ms) override;
    void StopAccepting() override;
    void set_owner_epoch(const std::string& epoch) override;
    bool Close(int grace_ms) override;

private:
    gateway::GatewayWorkPump* primary_ = nullptr;
    std::unique_ptr<gateway::GatewayWorkPump> secondary_;
};

// 渠道侧 GatewayWorkPump:TickOnce 推进所有已装配账号的桥泵与连接状态
// 输出/快照发布。
class ChannelGatewayWiring final : public gateway::GatewayWorkPump {
public:
    struct Options {
        const config::Config* config = nullptr;       // 全局配置(channels 段)
        std::filesystem::path channels_state_root;    // 渠道账号状态根(装配层给;
                                                       // =<状态根>/channels,个人布局
                                                       // ~/.lubancode/channels 原样)
        std::string ca_pem;                           // wss 信任锚;非空 = 显式信任锚
                                                       //(测试位/覆盖位,不回退平台
                                                       // 来源);空 = 平台默认信任根
        // Gateway 控制命令目录(Q1b 配对批准/拒绝的入站面;profile 树的
        // control/)。空 = 不接控制面(测试装配可不传)。
        std::filesystem::path gateway_control_dir;
        std::function<std::int64_t()> now_ms;
        // 测试注入位(生产恒空):网关传输工厂与 HTTP。空 = 生产件
        // (MakeWsTransportFactory/MakeDefaultHttpFunc)。装配后账号会起真
        // 网关线程——测试不注入即意味着真连生产端点,故必注入。
        std::function<std::unique_ptr<channel::transport::IGatewayTransport>()>
            test_transport_factory;
        channel::qq::QqHttpFunc test_http;
        // 飞书(F1)同款注入位(仅对 feishu 注册行生效)。
        std::function<std::unique_ptr<channel::feishu::IFeishuGatewayTransport>()>
            test_feishu_transport_factory;
        channel::feishu::FeishuHttpFunc test_feishu_http;
    };

    // 装配。channels 段为空时返回 nullptr(零渠道行为,不挂泵)。
    // 装配失败的账号记入 skipped(稳定码),不拦 Gateway 起来;信任根解析
    // 结果记入 diagnostics(cli 装配段打印,含"解析不到"的明报)。
    static std::unique_ptr<ChannelGatewayWiring> Create(Options options);

    ~ChannelGatewayWiring() override;

    bool TickOnce(std::int64_t now_ms) override;
    void StopAccepting() override;
    bool Close(int grace_ms) override;
    void set_owner_epoch(const std::string& epoch) override;

    // 观测(诊断/测试):不装配的渠道/账号与原因(稳定码);信任根诊断行。
    const std::vector<std::string>& skipped() const { return skipped_; }
    const std::vector<std::string>& diagnostics() const { return diagnostics_; }
    const channel::ChannelManager* manager() const { return manager_.get(); }
    // 渠道 work 泵(Q2)要的可变口:TakeNextWork/SendReply/结算面。
    channel::ChannelManager* mutable_manager() { return manager_.get(); }
    std::size_t adapter_count() const { return adapters_.size(); }
    // Q7:显式启用菜单/面板发布的账号数(观测/测试;同步结果看各账号
    // 状态文件 <state_root>/<ch>/<acct>/menu-panel.json 与 stderr 提示)。
    std::size_t menu_publisher_count() const { return menu_publishers_.size(); }

    // 挂渠道 work 泵(Q2:V3 与 outbox 总装)。TickOnce 在桥泵之后推进它
    //(先收字节回执,再驱动一轮业务);StopAccepting/Close/owner_epoch
    // 一并转发。生命周期归本件(先于 manager 析构)。
    void set_work_pump(std::unique_ptr<gateway::GatewayWorkPump> work_pump);

private:
    ChannelGatewayWiring() = default;
    void PumpAll();
    // Q1b 控制面:轮询 pairing 命令 → 应用到 manager(先按配对码认,认
    // 不出再按 sender 身份认)→ 写回执文件。
    void ConsumePairingCommands();
    // Q7 菜单/面板发布:显式启用的账号首拍同步 + 配对增删后的重同步;
    // 限速/失败按报告里的 retry_at 退避。HTTP 在 tick 里做(装配不碰网络)。
    void PumpMenuSync(std::int64_t now_ms);

    // Q7:显式启用发布的账号(菜单/面板)。publisher 持 http seam + 适配器
    // 的 token manager(单飞共用)。
    struct MenuPublisherEntry {
        std::string channel_id;
        std::string account_id;
        channel::ChannelMenuUserConfig menu;  // 期望配置(publish/panel.enabled 已含)
        std::unique_ptr<channel::qq::QqMenuPanelPublisher> publisher;
        bool pending = true;          // 首拍待同步
        std::int64_t retry_at_ms = 0;  // 失败/限速后的下一次可试
    };
    std::vector<MenuPublisherEntry> menu_publishers_;

    std::unique_ptr<channel::ChannelManager> manager_;
    std::vector<std::unique_ptr<channel::ChannelBridgeTransport>> adapters_;
    // adapter 的连接状态视图(所有权仍在 adapters_;注册行的装配产物递
    // 来,reporter 取快照用;快照是 channel 中立合同)。
    struct AdapterView {
        std::string channel_id;
        std::string account_id;
        std::function<channel::ConnectionSnapshot()> connection_state;
    };
    std::vector<AdapterView> adapter_views_;
    std::vector<std::string> skipped_;
    std::vector<std::string> diagnostics_;
    std::unique_ptr<gateway::GatewayWorkPump> work_pump_;
    std::unique_ptr<ChannelConnectionReporter> reporter_;
    std::string owner_epoch_;  // = Gateway boot_id(set_owner_epoch 递进)
    std::filesystem::path control_dir_;  // Q1b 配对控制面(空 = 不接)
};

}  // namespace lubancode::app
