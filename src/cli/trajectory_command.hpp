// `lubancode trajectory <verb> <session-id>`(P0-3 §十四命令族的分批落地):
//   verify        扫 session 目录逐流验链 + 父子边交叉核(不调模型不跑工具)
//   replay        main 的 exact replay:折叠 + 规范 state hash(§10.2)
//   harness-replay 录制桩重放宿主状态机(§10.3;live-rerun 属 P0-6+,不在此)
//   export        P0-5:Journal 投影成训练数据集,落 <session>/exports/
//                 training-v1/ 四路 + manifest(§3.1/§十一;只读本地账不联网)
//   export-workspace P0-5:逐 session 各导进各自 exports/,汇总报告
//   usage/gc/doctor 照 P0-4。退出码按结果。
#pragma once

#include <string>

namespace lubancode::cli {

struct TrajectoryCommandArgs {
    std::string verb;        // verify | replay | harness-replay | usage | gc | doctor |
                             // export | export-workspace
    std::string session_id;  // trajectory session id(trajectories/ 下那层);
                             // usage/gc/doctor/export-workspace 档当 workspace-key 用
    std::string trajectories_root;  // 空 = <home>/.lubancode/trajectories
    // gc 档:DryRun 只报账(默认);DerivedOnly 真删可重建/派生物(§12.2
    // 次序 temp→index→checkpoint→derived;canonical 与 artifacts 永不进候选)。
    bool gc_derived_only = false;
    // export/export-workspace 档:目标格式(cli_options 已钉只认 training-v1,
    // 这里再核一遍,双保险)。
    std::string format;
};

// 返回进程退出码:0 = 过;1 = 用法/找不到;2 = 验账/replay/导出未过。
int RunTrajectoryCommand(const TrajectoryCommandArgs& args);

}  // namespace lubancode::cli
