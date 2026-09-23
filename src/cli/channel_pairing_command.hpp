// channel pairing 子命令(QQ 接入单 Q1b):本地批准/拒绝控制入口。
//
// `lubancode channel pairing approve|reject <渠道> <账号> <配对码或身份>
// [--profile <名>] [--timeout <秒>]`——另一终端向持锁 Gateway 提交配对
// 裁决,经 Gateway 既有文件控制面(control/ 命令目录,stop/status 同款
// 通道)送达,回执文件带回结果。
//
// 投递门(纪律):普通交互进程不许拿一只空 manager 冒充批准成功——锁
// 不在/持有进程死透/锁读不懂都明确报错并指引先启动 Gateway,退码 2。
// 退出码:0 批准/拒绝成功;1 裁决失败(码过期/不认/已处理/账号未装配);
// 2 无持锁实例;3 命令投出但等不到回执(实例在但没消费——版本旧或
// 渠道未装配)。
#pragma once

#include <string>
#include <vector>

#include "cli/theme.hpp"

namespace lubancode::cli {

struct ChannelPairingCommandArgs {
    std::string action;       // "approve" | "reject"
    std::string channel_id;   // qqbot
    std::string account_id;   // main
    std::string token;        // 配对码或 sender 身份
    std::string profile;      // gateway profile;空 = default
    int timeout_ms = 10'000;  // 等回执的上限
};

// 投递门裁决(纯逻辑,测试直接喂):锁三态 + 活持有者。
enum class PairingGateStatus { Submit, NotRunning, StaleLock, BrokenLock };
struct ChannelPairingGate {
    PairingGateStatus status = PairingGateStatus::NotRunning;
    std::string detail;   // 人话(含指引;已脱敏)
    std::string boot_id;  // Submit 时 = 目标实例
    unsigned long pid = 0;
};
// lock_present = 锁文件在;lock_readable = 锁账读得懂;holder_alive =
// 持有进程活着(身份核过 token)。
ChannelPairingGate JudgePairingGate(bool lock_present, bool lock_readable,
                                    bool holder_alive, const std::string& boot_id,
                                    unsigned long pid);

// 命令入口(cli_app 调):锁探测 → 投命令 → 等回执 → 打印 → 退出码。
int RunChannelPairingCommand(const ChannelPairingCommandArgs& args);

// TUI 排版批 7:配对回执的 frame 渲染(纯函数,形状册直调)。既有两句
// ("已批准/已拒绝 <渠道>/<账号> 的配对身份: <身份>" / "提醒: …")原样
// 按冒号拆进键值对框,标题用命令名 channel pairing(schema 标识符)。
std::vector<std::string> RenderPairingReceipt(const std::string& action,
                                              const std::string& channel_id,
                                              const std::string& account_id,
                                              const std::string& sender_id, const Theme& theme,
                                              int width);

}  // namespace lubancode::cli
