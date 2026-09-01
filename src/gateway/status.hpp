// Gateway status 探测(总装单 G1):只读 probe,零写盘、零建目录。
//
// disabled 零副作用合同(contracts.md §6)的正面落点:普通 CLI 查状态时
// 绝不暗起 Gateway,也绝不留下任何目录或文件。探测三源:锁文件(谁持着、
// 身份核)、control.json(health/readiness 快照)、boot-history.jsonl(关机
// 连击/SafeMode 线索)。坏 control endpoint(文件在但读不懂)不崩,降级报
// gateway.control_unreachable 类诊断。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "gateway/control_server.hpp"
#include "gateway/process.hpp"

namespace lubancode::gateway {

struct GatewayProbe {
    enum class State {
        NotRunning,   // 没锁没活实例
        Running,      // 锁被活进程持有
        StaleLock,    // 锁在,持有者死透/PID 复用(下次启动自动清)
        StaleRemnant, // 锁没了但 control 还说 running(崩在关机半路/残留)
        BrokenLock,   // 锁文件读不懂:保守,不敢下结论
    };
    State state = State::NotRunning;
    GatewayLockRecord holder;                     // 锁里读到的账(有锁时)
    std::optional<GatewayControlSnapshot> control;  // 可空
    bool control_unreadable = false;              // control.json 在但读不懂
    std::string control_error;                    // 读不懂的说明
    int unclean_streak = 0;                       // 连续非干净关机连击
    std::string detail;                           // 人话总评
};

// 纯读探测。paths.root 不存在也照样回 NotRunning,不建任何东西。
GatewayProbe ProbeGateway(const GatewayProfilePaths& paths);

// `gateway status --json` 的正文(稳定字段名,机器可读)。
nlohmann::json ProbeToJson(const GatewayProbe& probe);

// 人话行(终端打印)。
std::vector<std::string> FormatProbeLines(const GatewayProbe& probe);

}  // namespace lubancode::gateway
