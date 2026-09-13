// `lubancode gateway <verb>`(总装单 G1 命令族 + V1 job 族):
//   run     前台真进程:取锁、写控制快照、接 V1 有界主泵(装配层递进,
//           见 args.pump)、等 stop(控制文件/SIGINT/SIGTERM),graceful
//           shutdown 后退。退出码 0 干净/2 已在跑/3 配置坏/4 关机超时。
//   status  只读 probe(锁 + control.json + boot history)+ V1 三栏领域
//           投影(work/execution/delivery),零写盘零建目录;--json 出
//           机器可读快照。
//   stop    投本地控制命令并等退出;不越权代杀(超时如实报)。
//   job     V1 持久任务入口:add/run-now 落控制命令文件(活着的 Gateway
//           消费进 AutomationStore,不直接改文件);list 只读账。
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
    bool json = false;    // status/job list --json
    // job 子族(verb == "job")。
    std::string job_verb;         // add | run-now | list
    std::string prompt;           // add 的任务正文
    std::string job_id;           // add --id / run-now 位置参数
    std::string idempotency_key;  // --idem
    long long due_at_ms = 0;      // add --at(0 = 立即)
    // V1 主泵(verb == "run" 时装配层递进;空 = G1 骨架行为,无业务面)。
    // 借用指针,须活过 RunGatewayCommand。
    gateway::GatewayWorkPump* pump = nullptr;
    // 测试/嵌入注入:空 = <home>/.lubancode/gateway。
    std::filesystem::path gateway_root;
};

// 返回进程退出码(合同见 docs/architecture/gateway/README.md §5)。
int RunGatewayCommand(const GatewayCommandArgs& args);

}  // namespace lubancode::cli
