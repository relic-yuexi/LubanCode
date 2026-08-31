// features.telemetry 激活合同(端云协同可观测架构与 Telemetry 插件设计单
// §八 默认关闭与激活合同,实施分期 T0"冻结 Activation")。
//
// 两只开关分开(§8.1):features.trajectory 管本地 canonical 账,
// features.telemetry 管观测投影。开 telemetry 不许暗开 trajectory(不变量
// 5);首版收窄只走 true/true 完整路,telemetry=true && trajectory=false
// 明确报 telemetry.requires_trajectory,不猜(§8.2/§33.1)。
//
// 环境变量(§8.3):
//   LUBANCODE_TELEMETRY=1|true|on|yes / 0|false|off|no  显式压配置一头
//   LUBANCODE_DISABLE_TELEMETRY=1                     紧急总闸:只关采集
//   发送,不改配置文件、不删 spool、不动 config.features_telemetry 字段
//
// 本件是纯判定:输入两枚配置布尔,自己读环境变量,回激活状态。不碰文件、
// 不起线程、不分配带副效应的资源——默认关闭零遥测副作用(§8.5)由调用
// 方按状态装配,未激活时一行 telemetry 代码都不跑。
#pragma once

#include <optional>
#include <string>

namespace lubancode::telemetry {

enum class TelemetryActivationStatus {
    Disabled,              // 配置/环境变量没开:旧路,零遥测副作用
    Active,                // trajectory=true && telemetry=true 完整路(T0 唯一支持)
    RequiresTrajectory,    // telemetry 开了 trajectory 没开:明确拒绝,不暗开
    DisabledByKillSwitch,  // LUBANCODE_DISABLE_TELEMETRY=1:采集发送全停
};

struct TelemetryActivation {
    TelemetryActivationStatus status = TelemetryActivationStatus::Disabled;
    // 稳定原因码(/telemetry status 与 /doctor telemetry 直接展示):
    //   telemetry.off / telemetry.env_off / telemetry.env_invalid_ignored
    //   telemetry.requires_trajectory / telemetry.kill_switch / telemetry.active
    std::string reason_code;
    bool config_telemetry = false;    // 传入的 features.telemetry(未改)
    bool config_trajectory = false;   // 传入的 features.trajectory(未改)
    std::optional<bool> env_opinion;  // LUBANCODE_TELEMETRY 的显式意见
    bool kill_switch = false;         // 总闸是否拉下

    bool enabled() const { return status == TelemetryActivationStatus::Active; }
};

// 合成激活状态。纯函数 + 环境变量读取,无其它副作用。
TelemetryActivation ResolveTelemetryActivation(bool config_telemetry,
                                               bool config_trajectory);

}  // namespace lubancode::telemetry
