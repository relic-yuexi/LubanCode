// 共用 Gateway 启动服务(§6.0"从 RunGateway 装配提取"):gateway run 与
// `lubancode im` 走同一份启动/锁/调度/停止合同,不复制运行时。
//
// GatewayLaunchPlan 是一次启动的不可变选择:profile、渠道范围(单账号
// 过滤器只成启动过滤器,不改全局 enabled)、自动任务开关。不含
// AppSecret/token,不修改全局配置。
//
// 结构修复(§6.0):渠道装配不再以"先有 automation pump"为前提——
// require_automation_pump=false 时,automation 泵开不了也能起渠道侧
// (outbox 以独立件打开,渠道 work 泵照挂),不让旧装配关系成为 IM 的
// 隐藏依赖。gateway run 维持既有语义(automation 泵开不了就退 1)。
#pragma once

#include <optional>
#include <string>

#include "config/config.hpp"

namespace lubancode::app {

// 渠道账号引用(im 的选择结果/启动过滤器)。
struct ChannelAccountRef {
    std::string channel_id;
    std::string account_id;
};

struct GatewayLaunchPlan {
    std::string profile;  // 空 = default
    // gateway run 语义:automation 泵开不了是硬失败(退 1,文案不变);
    // false:继续起渠道侧(outbox 独立打开)。im 当前也用 true(IM 不以
    // 聊天入口为由关自动任务,§6.0),false 留给"只跑渠道"的场合。
    bool require_automation_pump = true;
    // 单账号启动过滤器:只装配/投递这个渠道账号,其余账号的 enabled 一律
    // 不动(过滤器只活在本次进程的装配输入里)。nullopt = 全部已启用渠道。
    std::optional<ChannelAccountRef> channel_account_filter;
};

// 纯函数:按过滤器裁 channels 段(其余配置字段不动)。不过滤时原样返回
// 副本。渠道层 bindings/tools/default_account 保留(渠道层账,不过账号)。
config::Config FilterConfigForChannelAccount(const config::Config& config,
                                             const std::optional<ChannelAccountRef>& filter);

// 装配 + 跑(阻塞到 Gateway 退出)。返回进程退出码(与 gateway run 同一
// 合同:0 干净/2 已在跑/3 配置坏/4 关机超时)。诊断打印与原 RunGateway
// 内联装配逐字相同,只挪了结构。
int RunGatewayWithPlan(const GatewayLaunchPlan& plan);

}  // namespace lubancode::app
