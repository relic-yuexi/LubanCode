// `lubancode gateway <verb>`(总装单 G1 命令族 + V1 job 族):
//   run     前台真进程:取锁、写控制快照、接 V1 有界主泵(装配层递进,
//           见 args.pump)、等 stop(控制文件/SIGINT/SIGTERM),graceful
//           shutdown 后退。退出码 0 干净/2 已在跑/3 配置坏/4 关机超时。
//   status  只读 probe(锁 + control.json + boot history)+ V1 三栏领域
//           投影(work/execution/delivery),零写盘零建目录;--json 出
//           机器可读快照。
//   stop    投本地控制命令并等退出;不越权代杀(超时如实报)。
//   job     持久任务入口(V1 add/run-now;V2 update/pause/resume/cancel/
//           import-loop 与 read):写操作落控制命令文件(活着的 Gateway
//           消费进 AutomationStore,不直接改文件);list/read 只读账。
// install/start/restart/doctor/logs 是 G2+ 的口,不在此冒充。
#pragma once

#include <filesystem>
#include <string>

#include "gateway/profile.hpp"
#include "gateway/work_pump.hpp"

namespace lubancode::cli {

struct GatewayCommandArgs {
    std::string verb;     // run | status | stop | job
    std::string profile;  // 空 = default
    bool json = false;    // status/job list/job read --json
    // job 子族(verb == "job")。
    std::string job_verb;         // add | run-now | list | read | update | pause | resume |
                                  // cancel | import-loop
    std::string prompt;           // add/update/import-loop 的任务正文
    std::string job_id;           // add --id / run-now / read / update / pause / resume /
                                  // cancel 位置参数
    std::string idempotency_key;  // --idem
    long long due_at_ms = 0;      // add --at(0 = 立即)
    // V2 计划与领域操作参数。
    long long interval_seconds = 0;  // --every(add/update/import-loop;秒)
    std::string cron_expr;           // --cron(五字段受限子集)
    std::string timezone;            // --tz(缺省 UTC;显式存储)
    std::string misfire;             // --misfire(coalesce|skip;缺省 coalesce)
    long long deadline_ms = 0;       // --deadline(0 = 不设)
    bool heartbeat = false;          // --heartbeat(notify_on_change)
    long long expected_revision = 0; // --rev(update/pause/resume/cancel 的 CAS)
    std::string source_session_id;   // import-loop 位置参数(原 /loop 会话)
    std::string source_task_id;      // import-loop --task(原 loop-N)
    // V1 主泵(verb == "run" 时装配层递进;空 = G1 骨架行为,无业务面)。
    // 借用指针,须活过 RunGatewayCommand。
    gateway::GatewayWorkPump* pump = nullptr;
    // 测试/嵌入注入:空 = gateway 状态根(gateway::DefaultGatewayRoot(),
    // 即 <状态根>/gateway;个人布局=~/.lubancode/gateway 原样)。
    std::filesystem::path gateway_root;
};

// 返回进程退出码(合同见 docs/architecture/gateway/README.md §5)。
int RunGatewayCommand(const GatewayCommandArgs& args);

}  // namespace lubancode::cli
