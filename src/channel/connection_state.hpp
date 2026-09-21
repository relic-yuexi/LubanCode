// 渠道中立连接状态合同(架构审查 SV-07,自 qq_adapter.hpp 迁中立位):
// 三平台(QQ/飞书/企微)适配器透传的结构化连接快照,宿主(注册表/
// reporter/CLI)按字段消费同一份类型,不各养一份猜测状态,也不逐字段
// 手抄换名。合同只定字段与类型;平台语义归平台——QQ READY/RESUMED、
// 飞书 connected、企微订阅成功的"connected=true"判定各归各的网关,
// stage 取各平台 kStage* 稳定名(未启动 = "idle"),本头不定阶段名。
//
// 脱敏口径:字段全部来自网关事件的稳定账(阶段/稳定码/脱敏说明),不碰
// 凭据,不落 token/secret/原始响应体。
//
// 迁移兼容:qq/wecombot 命名空间各留 ConnectionSnapshot/ConnectionFailure
// 迁移期别名(qq_adapter.hpp/wecom_adapter.hpp),调用方全数改用 channel::
// 命名后即可退役。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::channel {

// 一笔连接失败:失败阶段 + 稳定错误码 + 脱敏说明。
struct ConnectionFailure {
    std::string stage;       // 失败发生阶段(各平台 kStage*)
    std::string error_code;  // 稳定码
    std::string detail;      // 脱敏说明
    std::int64_t at_ms = 0;
    int attempt = 0;         // 尝试编号(与 BackoffScheduled.attempt 同轮)
};

// 连接状态快照(连接状态单 §三:网关独占平台连接状态,适配器透传结构化
// 快照,宿主负责输出;快照是状态不是事件,不冒充失败次数)。
struct ConnectionSnapshot {
    bool thread_alive = false;   // 网关线程存活(≠ connected)
    bool connected = false;      // 只在平台判定在线后 true;断线/停止立即 false
    std::string stage;           // 当前阶段(各平台 kStage*;未启动 = "idle")
    std::optional<ConnectionFailure> last_failure;   // 当前最近失败(连接成功后清)
    std::vector<ConnectionFailure> failure_history;  // 成功时归档(留最近 8 笔)
    int retry_count = 0;             // BackoffScheduled 的 attempt
    std::int64_t next_retry_at_ms = 0;
    std::int64_t connected_since_ms = 0;  // 本轮在线起点(0 = 未在线)
    std::int64_t updated_at_ms = 0;       // 快照记账时刻
};

}  // namespace lubancode::channel
