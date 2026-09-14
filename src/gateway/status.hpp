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

// ---------------------------------------------------------------------------
// V1 分栏(单子 V1 第五件事):process / work / execution / delivery 四栏
// 分报——进程 running 不冒充任务成功。后三栏是领域账的只读投影(账不在
// = 空栏,不建目录零写盘),与 process 栏的活探针分开采、分开报。
// ---------------------------------------------------------------------------

struct GatewayStatusSections {
    // work 栏:任务与 occurrence 的账面状态。
    std::size_t jobs_total = 0;
    std::size_t occurrences_due = 0;        // scheduled 且到点(等 claim)
    std::size_t occurrences_in_flight = 0;  // claimed 未结算
    std::size_t occurrences_succeeded = 0;
    std::size_t occurrences_failed = 0;
    std::size_t occurrences_needs_review = 0;
    // execution 栏:最近一枚 occurrence 的执行落点(空 = 还没跑过)。
    struct ExecutionEntry {
        std::string occurrence_id;
        std::string job_id;
        std::string session_id;   // V3 场(绑定账;未绑定为空)
        std::string turn_id;
        std::string outcome;      // 空 = 未结算(in_flight)
    };
    std::vector<ExecutionEntry> recent_executions;  // 最近若干枚(倒序)
    // delivery 栏:outbox 投影。
    std::size_t delivery_pending = 0;
    std::size_t delivery_delivered = 0;
    std::size_t delivery_flagged = 0;
    std::vector<std::string> pending_delivery_ids;
    bool work_ledger_present = false;   // automation 账在不在(空栏与坏账分得开)
    bool delivery_ledger_present = false;
    // channel 栏(QQ 接入单 Q2 §七末行):渠道连接与投递错误——只读盘上
    // 账(account-status.json + ingress/outbox 投影),不带密钥与整份平台
    // 事件。
    struct ChannelAccountEntry {
        std::string channel_id;
        std::string account_id;
        std::string connection_state;  // 账号状态机快照(空 = 快照未见)
        int generation = 0;
        std::size_t ingress_pending = 0;  // queued + running
        std::size_t dead_letter = 0;
        std::vector<std::string> delivery_errors;  // 渠道投递终态失败(稳定码)
    };
    std::vector<ChannelAccountEntry> channels;
    bool channel_ledger_present = false;  // channels 状态根在不在
};

// 三栏只读投影(process 栏仍在 GatewayProbe)。纯读,零建目录零写盘。
GatewayStatusSections ProbeStatusSections(const GatewayProfilePaths& paths);

// 分栏的 JSON 面(挂在 ProbeToJson 的 "work"/"execution"/"delivery" 键下)。
nlohmann::json SectionsToJson(const GatewayStatusSections& sections);

// 分栏的人话行(终端打印,前缀分栏)。
std::vector<std::string> FormatSectionLines(const GatewayStatusSections& sections);

// `gateway status --json` 的正文(稳定字段名,机器可读)。
nlohmann::json ProbeToJson(const GatewayProbe& probe);

// 人话行(终端打印)。
std::vector<std::string> FormatProbeLines(const GatewayProbe& probe);

}  // namespace lubancode::gateway
