// `lubancode trajectory <verb> <session-id>`(P0-3 §十四命令族的 P0-3 档):
//   verify        扫 session 目录逐流验链 + 父子边交叉核(不调模型不跑工具)
//   replay        main 的 exact replay:折叠 + 规范 state hash(§10.2)
//   harness-replay 录制桩重放宿主状态机(§10.3;live-rerun 属 P0-4+,不在此)
// inspect/export 的富输出随 P0-4/P0-5 落;这里只做只读诊断,退出码按结果。
#pragma once

#include <string>

namespace lubancode::cli {

struct TrajectoryCommandArgs {
    std::string verb;        // verify | replay | harness-replay
    std::string session_id;  // trajectory session id(trajectories/ 下那层)
    std::string trajectories_root;  // 空 = <home>/.lubancode/trajectories
};

// 返回进程退出码:0 = 过;1 = 用法/找不到;2 = 验账/replay 未过。
int RunTrajectoryCommand(const TrajectoryCommandArgs& args);

}  // namespace lubancode::cli
