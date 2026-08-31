// `lubancode trajectory <verb> <session-id>`(P0-3 §十四命令族的 P0-3 档):
//   verify        扫 session 目录逐流验链 + 父子边交叉核(不调模型不跑工具)
//   replay        main 的 exact replay:折叠 + 规范 state hash(§10.2)
//   harness-replay 录制桩重放宿主状态机(§10.3;live-rerun 属 P0-4+,不在此)
// inspect/export 的富输出随 P0-4/P0-5 落;这里只做只读诊断,退出码按结果。
#pragma once

#include <string>

namespace lubancode::cli {

struct TrajectoryCommandArgs {
    std::string verb;        // verify | replay | harness-replay | usage | gc | doctor
    std::string session_id;  // trajectory session id(trajectories/ 下那层);
                             // usage/gc/doctor 档当 workspace-key 用
    std::string trajectories_root;  // 空 = <home>/.lubancode/trajectories
    // gc 档:DryRun 只报账(默认);DerivedOnly 真删可重建/派生物(§12.2
    // 次序 temp→index→checkpoint→derived;canonical 与 artifacts 永不进候选)。
    bool gc_derived_only = false;
};

// 返回进程退出码:0 = 过;1 = 用法/找不到;2 = 验账/replay 未过。
int RunTrajectoryCommand(const TrajectoryCommandArgs& args);

}  // namespace lubancode::cli
