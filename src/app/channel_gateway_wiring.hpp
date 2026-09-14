// Gateway 的渠道装配与复合泵(QQ 机器人接入单 Q1,Q0 留件"激活接线与
// trust 快照装配"的落地)。
//
// 装配面(todo §五/Q0 记账):gateway run 启动路径读全局 channels 段,对每只
// 启用账号跑唯一权威激活函数 ResolveChannelActivation——五闸过了才把
// ChannelManager::AddAccount + StartAccount 真接上。QQ 定案进程内直连
// (§十五):trust 快照按"渠道实现内置受信"恒信任(不造假包概念),凭据经
// Q0 resolver 解析后留在宿主进程内,适配器是 ChannelBridgeTransport 的
// 进程内实现。其余渠道(Q1 未实现适配器)如实记 skipped,不起任何线程。
//
// 复合泵:GatewayProcess::Options 只收一只泵,渠道的 Pump(出站帧 flush +
// 入站字节 drain)是有界同步件,与 automation 主泵同 tick——本件把两只
// GatewayWorkPump 叠一只,不动 engine 冻结面。
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "channel/manager.hpp"
#include "channel/qq/qq_gateway.hpp"
#include "channel/qq/qq_http.hpp"
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

// 渠道侧 GatewayWorkPump:TickOnce 推进所有已装配账号的桥泵。
class ChannelGatewayWiring final : public gateway::GatewayWorkPump {
public:
    struct Options {
        const config::Config* config = nullptr;       // 全局配置(channels 段)
        std::filesystem::path channels_state_root;    // 渠道账号状态根(装配层给;
                                                       // =<状态根>/channels,个人布局
                                                       // ~/.lubancode/channels 原样)
        std::string ca_pem;                           // wss 信任锚;空 = 探测平台 PEM
        std::function<std::int64_t()> now_ms;
        // 测试注入位(生产恒空):网关传输工厂与 HTTP。空 = 生产件
        // (MakeWsTransportFactory/MakeDefaultHttpFunc)。装配后账号会起真
        // 网关线程——测试不注入即意味着真连生产端点,故必注入。
        std::function<std::unique_ptr<channel::qq::IGatewayTransport>()>
            test_transport_factory;
        channel::qq::QqHttpFunc test_http;
    };

    // 装配。channels 段为空时返回 nullptr(零渠道行为,不挂泵)。
    // 装配失败的账号记入 skipped(稳定码),不拦 Gateway 起来。
    static std::unique_ptr<ChannelGatewayWiring> Create(Options options);

    ~ChannelGatewayWiring() override;

    bool TickOnce(std::int64_t now_ms) override;
    void StopAccepting() override;
    bool Close(int grace_ms) override;
    void set_owner_epoch(const std::string& epoch) override;

    // 观测(诊断/测试):不装配的渠道/账号与原因(稳定码)。
    const std::vector<std::string>& skipped() const { return skipped_; }
    const channel::ChannelManager* manager() const { return manager_.get(); }
    // 渠道 work 泵(Q2)要的可变口:TakeNextWork/SendReply/结算面。
    channel::ChannelManager* mutable_manager() { return manager_.get(); }
    std::size_t adapter_count() const { return adapters_.size(); }

    // 挂渠道 work 泵(Q2:V3 与 outbox 总装)。TickOnce 在桥泵之后推进它
    //(先收字节回执,再驱动一轮业务);StopAccepting/Close/owner_epoch
    // 一并转发。生命周期归本件(先于 manager 析构)。
    void set_work_pump(std::unique_ptr<gateway::GatewayWorkPump> work_pump);

private:
    ChannelGatewayWiring() = default;
    void PumpAll();

    std::unique_ptr<channel::ChannelManager> manager_;
    std::vector<std::unique_ptr<channel::ChannelBridgeTransport>> adapters_;
    std::vector<std::string> skipped_;
    std::unique_ptr<gateway::GatewayWorkPump> work_pump_;
};

}  // namespace lubancode::app
