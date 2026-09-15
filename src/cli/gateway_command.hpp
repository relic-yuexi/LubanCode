// `lubancode gateway <verb>`(总装单 G1 命令族 + V1 job 族 + V4 运维族):
//   run     前台真进程:取锁、写控制快照、接 V1 有界主泵(装配层递进,
//           见 args.pump)、等 stop(控制文件/SIGINT/SIGTERM),graceful
//           shutdown 后退。退出码 0 干净/2 已在跑/3 配置坏/4 关机超时。
//   status  只读 probe(锁 + control.json + boot history)+ V1 三栏领域
//           投影(work/execution/delivery),零写盘零建目录;--json 出
//           机器可读快照。
//   stop    投本地控制命令并等退出;不越权代杀(超时如实报)。这也是
//           supervisor 语境下的停止语义(V4:uninstall/restart 的收口
//           都先走它)。
//   job     持久任务入口(V1 add/run-now;V2 update/pause/resume/cancel/
//           import-loop 与 read):写操作落控制命令文件(活着的 Gateway
//           消费进 AutomationStore,不直接改文件);list/read 只读账。
//   install     (V4)生成平台服务单元(Windows schtasks XML/systemd user
//               unit/macOS launchd plist)并注册;exe 路径/工作目录/
//               参数落死;install 前校验配置可装载;不 start。
//   uninstall   (V4)先文件面 stop(drain 语义与 gateway stop 统一)再摘
//               服务;账与 boot history 保留。
//   start       (V4)经服务管理器拉起(schtasks /Run|systemctl --user
//               start|launchctl load/kickstart),不裸 spawn;未 install
//               明错。
//   restart     (V4)文件面 stop → 服务面 start(先 reconcile 后接新活由
//               泵的恢复扫描保证:TickOnce 恢复先于新派发)。
//   doctor      (V4)体检清单,一项一码;--wait-ready <秒> 健康探针;
//               --ack-safe-mode 显式确认清 SafeMode 连击;退出码 0/1/2
//               (全绿/有警/有病),--wait-ready 超时退 1。
//   logs        (V4)boot history 与 gateway.log 尾部 + 服务 stdout/
//               stderr 落位指引(不做日志聚合)。
#pragma once

#include <filesystem>
#include <string>

#include "gateway/profile.hpp"
#include "gateway/work_pump.hpp"

namespace lubancode::cli {

struct GatewayCommandArgs {
    std::string verb;     // run | status | stop | job | install | uninstall | start |
                          // restart | doctor | logs
    std::string profile;  // 空 = default
    bool json = false;    // status/job list/job read/doctor --json
    // job 子族(verb == "job")。
    std::string job_verb;         // add | run-now | list | read | update | pause |
                                  // resume | cancel | import-loop
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
    // V4 运维族参数。
    int wait_ready_secs = 0;     // doctor --wait-ready(0 = 不等)
    bool ack_safe_mode = false;  // doctor --ack-safe-mode
    int tail_lines = 20;         // logs --tail(缺省 20)
    // V1 主泵(verb == "run" 时装配层递进;空 = G1 骨架行为,无业务面)。
    // 借用指针,须活过 RunGatewayCommand。
    gateway::GatewayWorkPump* pump = nullptr;
    // 测试/嵌入注入:空 = gateway 状态根(gateway::DefaultGatewayRoot(),
    // 即 <状态根>/gateway;个人布局=~/.lubancode/gateway 原样);install
    // 把它钉进服务单元(--gateway-root 显式指)。
    std::filesystem::path gateway_root;
};

// 返回进程退出码(合同见 docs/architecture/gateway/README.md §5 与
// contracts.md §14)。
int RunGatewayCommand(const GatewayCommandArgs& args);

}  // namespace lubancode::cli
