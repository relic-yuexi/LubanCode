// `lubancode gateway <verb>`(总装单 G1 命令族的首页):
//   run     前台真进程:取锁、写控制快照、等 stop(控制文件/SIGINT/SIGTERM),
//           graceful shutdown 后退。退出码 0 干净/2 已在跑/3 配置坏/4 关机超时。
//   status  只读 probe(锁 + control.json + boot history),零写盘零建目录;
//           --json 出机器可读快照。
//   stop    投本地控制命令并等退出;不越权代杀(超时如实报)。
// install/start/restart/doctor/logs 是 G2+ 的口,不在此冒充。
#pragma once

#include <filesystem>
#include <string>

#include "gateway/profile.hpp"

namespace lubancode::cli {

struct GatewayCommandArgs {
    std::string verb;     // run | status | stop
    std::string profile;  // 空 = default
    bool json = false;    // status --json
    // 测试/嵌入注入:空 = <home>/.lubancode/gateway。
    std::filesystem::path gateway_root;
};

// 返回进程退出码(合同见 docs/architecture/gateway/README.md §5)。
int RunGatewayCommand(const GatewayCommandArgs& args);

}  // namespace lubancode::cli
