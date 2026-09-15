// Gateway doctor(总装单 V4 §十 第三行):体检清单——服务注册状态、锁/
// 实例活态、凭据可读(不显值)、账号激活闸(配置面)、磁盘可写、坏账扫描
// (V2 needs_review 面)、死信计数、SafeMode、安装记录对账。一项一码零
// 猜测:码表冻结于 contracts.md §14,退出码语义稳定(0 全绿/1 有警/
// 2 有病)。
//
// 健康探针:`gateway doctor --wait-ready <秒>` 供 install 后验证与外部
// 监控——轮询 ProbeGateway 至"锁被活进程持有 + control 报 running +
// health ok + 非 SafeMode",超时如实退 1,不假报 ready。
//
// SafeMode 显式 ack 口(contracts §10.3 留给 V4 doctor):--ack-safe-mode
// 往 boot-history 落一行 type=ack,效力同干净关机(连击清零),但不伪造
// shutdown 事实——账上仍看得出这是一次人工确认。
//
// 零副作用边界:doctor 无 --ack-safe-mode 时零写盘零建目录(profile 目
// 录不存在就报 disk.no_profile_dir,不建);disk 探针写在既有目录内且
// 即写即删。
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "gateway/profile.hpp"
#include "gateway/service.hpp"
#include "gateway/status.hpp"

namespace lubancode::gateway {

enum class DoctorSeverity { Ok, Info, Warn, Fail };
const char* DoctorSeverityName(DoctorSeverity severity);

// 一项体检。code 是稳定码(contracts §14 码表);detail 人话(脱敏:
// 凭据检查绝不带值)。
struct DoctorCheck {
    std::string code;
    DoctorSeverity severity = DoctorSeverity::Info;
    std::string detail;

    nlohmann::json ToJson() const;
};

struct DoctorReport {
    std::vector<DoctorCheck> checks;
    int unclean_streak = 0;  // SafeMode 连击(诊断参考)

    bool HasFail() const;
    bool HasWarn() const;
    // 退出码(冻结):0 = 无 Warn 无 Fail;1 = 有 Warn;2 = 有 Fail。
    int ExitCode() const;
    nlohmann::json ToJson() const;
    std::vector<std::string> FormatLines() const;
};

struct DoctorOptions {
    // 服务管理器探测(注册状态)。空 = 跳过该面(报 service.skipped)。
    ServiceRunner service_runner;
    // 平台(服务注册探测的命令形状)。缺省当前编译平台。
    ServicePlatform platform = CurrentServicePlatform();
    // channels 配置读取 seam:返回用户 config.json 全文(UTF-8)。
    // 空 = 跳过凭据面(报 credentials.skipped)。生产默认读全局 config。
    std::function<std::optional<std::string>()> read_channels_config;
    // 当前 lubancode 版本(install 记录对账;空 = 跳过版本比对)。
    std::string lubancode_version;
    // 磁盘写探针 seam(测试注入只读面)。缺省 = 真写临时文件再删。
    std::function<bool(const std::filesystem::path& dir)> write_probe;
    std::function<std::int64_t()> now_ms;
};

// 跑一遍体检。纯读(除 disk 探针的即写即删),零建目录。
DoctorReport RunGatewayDoctor(const GatewayProfilePaths& paths, const DoctorOptions& options);

// ---------------------------------------------------------------------------
// 健康探针(--wait-ready)
// ---------------------------------------------------------------------------

// ready 判定(纯函数):锁被活进程持有 + control 快照在且 state=running +
// health=ok + 非 SafeMode。进程活着不算 ready,业务面暂停(SafeMode)
// 也不算。
bool IsGatewayReady(const GatewayProbe& probe);

struct WaitReadyOutcome {
    bool ready = false;
    std::string detail;
    std::int64_t waited_ms = 0;
};

// 轮询等 ready,至多 timeout_secs。sleep_ms seam 供测试注入(生产真睡)。
WaitReadyOutcome WaitForGatewayReady(const GatewayProfilePaths& paths, int timeout_secs,
                                     const std::function<std::int64_t()>& now_ms,
                                     const std::function<void(int)>& sleep_ms);

// ---------------------------------------------------------------------------
// SafeMode 显式 ack(--ack-safe-mode)
// ---------------------------------------------------------------------------

// 往 boot-history 落一行 type=ack(reason=ack_safe_mode):连击清零,效力
// 同干净关机,但不伪造 shutdown 事实。返回空 = 记上了。
std::string AckSafeMode(const GatewayProfilePaths& paths);

}  // namespace lubancode::gateway
