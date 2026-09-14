// GatewayWorkPump(常驻总装 V1 第一件事的合同口):GatewayProcess 主循环
// 接的有界业务泵。
//
// 分层规矩:process.cpp 在 engine 层,不 include runtime/app(单子 §三
// 的既有边界);业务泵的真装配(automation/headless 执行器/outbox)在
// runtime 层实现本接口,由 CLI 装配层递进 GatewayProcess::Options。与
// ShutdownHook 同款机制,业务面从 G2 起挂进主循环的先例在此兑现。
//
// 有界合同:TickOnce 至多推进一枚执行(V1 单飞)+ 一轮 outbox 投递 +
// 一轮恢复扫描;同步收口,返回后主循环才查下一遍停止命令(V1 取舍:
// 泵同步跑,stop 在 turn 边界生效——宽限内收不净按既有 shutdown_timeout
// 纪律如实记账,不假写 clean)。
//
// 收尾次序(单子 V1 第一件事):先暂停接活(StopAccepting:不再受理新
// 命令/不再认领新 occurrence)→ 主循环已出 = 摘 wake(不再 TickOnce)
// → Close 收执行器与领域 writer(cancel 在飞 turn 的取消旗在实现侧,
// V1 同步泵在 Close 时无在飞)。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::gateway {

class GatewayWorkPump {
public:
    virtual ~GatewayWorkPump() = default;

    // 主循环每 tick 调一次(StopAccepting 后不再调)。返回 false = 泵
    // broken(账写不进等),主循环记日志并停止业务 tick;进程不因此退出
    //(控制面仍活,operator 看日志处置)。
    virtual bool TickOnce(std::int64_t now_ms) = 0;

    // 暂停接活(关机次序第一步):此后 TickOnce 只做在飞收尾,不取新活。
    virtual void StopAccepting() = 0;

    // 实例 fencing 代号:GatewayProcess 取到锁后把锁内 owner_epoch(=
    // boot_id)递进来;泵把它写进 occurrence.claimed(§11.1 ownerEpoch 拦
    // 旧 worker 迟到提交)。缺省 no-op(测试装配可自给)。
    virtual void set_owner_epoch(const std::string& epoch) { (void)epoch; }

    // 收执行器与领域 writer(关机次序最后一步)。grace_ms 内收净回 true;
    // 没收净回 false(调用方记 gateway.shutdown_timeout,不假写 clean)。
    virtual bool Close(int grace_ms) = 0;
};

// ---------------------------------------------------------------------------
// job 控制命令文件(本地命令面,单子 V1 第二件事:"持久任务创建走本地
// 命令/控制服务,不直接改文件"——用户侧入口是 CLI,CLI 落命令文件,
// 活着的 Gateway 消费进 AutomationStore)。
//
// 形态同 stop.json:命令落在 control/ 目录,泵轮询消费即删。job 命令
// 要串行多枚,一枚一文件:control/job-add-<pid>-<seq>.json、
// control/job-run-now-<pid>-<seq>.json。文件名不承载语义,只防互踩;
// 消费侧按内容处理,读不懂的命令文件删除并留日志(不追杀,同 stop)。
// ---------------------------------------------------------------------------

// 计划形态的命令载荷(V2):--at/--every/--cron/--tz/--misfire/--deadline
// 折进 add;update 只带要改的键(set_* 由 CLI 按出现折)。
struct GatewayJobSchedulePatch {
    bool set_due_at = false;
    std::int64_t due_at_ms = 0;         // once
    bool set_interval = false;
    std::int64_t interval_seconds = 0;  // interval
    bool set_cron = false;
    std::string cron_expr;              // cron(五字段受限子集)
    bool set_timezone = false;
    std::string timezone;               // IANA 名或 UTC±H[:MM];显式存储
    bool set_misfire = false;
    std::string misfire;                // coalesce | skip
    bool set_deadline = false;
    std::int64_t deadline_ms = 0;
    bool set_notify_on_change = false;
    bool notify_on_change = false;      // heartbeat 口:结果未变不投递
};

struct GatewayJobAddCommand {
    int schema_version = 2;
    std::string prompt;            // 任务正文
    std::string idempotency_key;   // 调用方幂等键(重发同键)
    std::string job_id;            // 可选指名;空 = 服务发号
    std::int64_t due_at_ms = 0;    // once:0 = 立即(V1 语义)
    std::int64_t requested_at_ms = 0;
    GatewayJobSchedulePatch schedule;  // V2:--every/--cron 等出现即设

    nlohmann::json ToJson() const;
};

struct GatewayJobRunNowCommand {
    int schema_version = 1;
    std::string job_id;
    std::string idempotency_key;
    std::int64_t requested_at_ms = 0;

    nlohmann::json ToJson() const;
};

// 领域操作(V2 第二件事):一律带 expected_revision(CAS;0 = 拒)与
// 幂等键。pause/resume/cancel 同形,靠 verb 分。
struct GatewayJobUpdateCommand {
    int schema_version = 1;
    std::string job_id;
    std::uint64_t expected_revision = 0;
    std::string idempotency_key;
    std::string prompt;                 // 空 = 不改
    GatewayJobSchedulePatch schedule;   // set_* 带要改的键
    std::int64_t requested_at_ms = 0;

    nlohmann::json ToJson() const;
};

struct GatewayJobStateCommand {
    int schema_version = 1;
    std::string verb;  // pause | resume | cancel
    std::string job_id;
    std::uint64_t expected_revision = 0;
    std::string idempotency_key;
    std::int64_t requested_at_ms = 0;

    nlohmann::json ToJson() const;
};

// /loop 显式导入(V2 第五件事):产 receipt——无 receipt 不暗搬、不双跑。
struct GatewayJobImportLoopCommand {
    int schema_version = 1;
    std::string source_session_id;  // 原 /loop 所在会话
    std::string source_task_id;     // 原 loop-N
    std::string prompt;             // 固定副本(不逐拍现读原状态)
    std::int64_t interval_seconds = 0;
    std::string idempotency_key;
    std::int64_t requested_at_ms = 0;

    nlohmann::json ToJson() const;
};

// CLI 写侧:落一枚 job 命令文件(名字带 pid+序号防互踩)。返回空 = 成功。
std::string WriteJobAddCommand(const std::filesystem::path& control_dir,
                               const GatewayJobAddCommand& command);
std::string WriteJobRunNowCommand(const std::filesystem::path& control_dir,
                                  const GatewayJobRunNowCommand& command);
std::string WriteJobUpdateCommand(const std::filesystem::path& control_dir,
                                  const GatewayJobUpdateCommand& command);
std::string WriteJobStateCommand(const std::filesystem::path& control_dir,
                                 const GatewayJobStateCommand& command);
std::string WriteJobImportLoopCommand(const std::filesystem::path& control_dir,
                                      const GatewayJobImportLoopCommand& command);

// 泵消费侧:扫 control/ 下 job- 前缀的命令文件,读、删(读不懂也删,
// 不追杀)。目录不存在 = 空结果(零副作用;status 等只读命令不建目录)。
struct ConsumedJobCommands {
    std::vector<GatewayJobAddCommand> adds;
    std::vector<GatewayJobRunNowCommand> run_nows;
    std::vector<GatewayJobUpdateCommand> updates;
    std::vector<GatewayJobStateCommand> state_ops;  // pause/resume/cancel
    std::vector<GatewayJobImportLoopCommand> import_loops;
    std::size_t discarded = 0;  // 读不懂/认不出删掉的文件数(诊断)
};
ConsumedJobCommands PollJobCommands(const std::filesystem::path& control_dir);

}  // namespace lubancode::gateway
