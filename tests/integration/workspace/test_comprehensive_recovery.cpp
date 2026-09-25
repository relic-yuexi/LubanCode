// Workspace 收官验收·综合恢复册(单子 §一第 1 条):
//   主会话 + 四只并行子代理 + 嵌套 child + workflow + Memory recall/save
//   的完整现场,杀进程(句柄全丢 + 死 PID 陈旧锁 + 尾行撕裂)与断流
//  (回合半路无输出、子账半行)之后:
//     - RecoverWorkspace 以 Journal 可证事实收口(旧场 incomplete、
//       session.json 补正、旧 main 一个字节不再追加);
//     - ResumeAsNew 七步接上(新场 start_reason=resume,悬空分档如实);
//     - VerifySessionDir 全流验链:撕裂流报 verify.truncated_tail,其余全过;
//     - Memory 住 workspace 树,换场不丢——新场 recall 照常命中;
//     - 恢复后的新场能继续干活(再写一轮、再验)。
// 与 unit/trajectory/test_session_manager_recovery.cpp 的分工:那册拆
// clear 八步的各崩溃点;本册拼完整生产形状(main+4 并行子+嵌套+workflow+
// memory),验收线是"全要素现场杀进程后 resume/verify 全过"。
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "app/memory_ledger_bridge.hpp"
#include "memory/project_memory.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/session_lock.hpp"
#include "trajectory/session_manager.hpp"

using namespace lubancode;
using trajectory::Actor;
using trajectory::Durability;
using trajectory::EventKind;
using trajectory::RecordReceipt;

// (V3-LEGACY-01:原匿名命名空间内的 CrashRig/夹具 helper 随案退役删除)


// (退役,V3-LEGACY-01)原此处有"综合恢复"两案(全要素现场杀进程 + 干净
// 封口后崩溃无痕):靠注入 0 开 v2 活场,现场织入 memory 召回、四只并行
// 子代理、嵌套派工与 workflow 编排账,再验 recover/resume/verify/字节不动
// 全链。写口退役后 v2 活场造不出,且其中的 SpawnWorkflowRun 旧桥对 v3 活场
// 在 trajectory_workflow_bridge.cpp:510 解引用空 main(存量隐患,见销项册
// V3-LEGACY-01 边界注)——本案无法在 v3 下原样重织。各分域的 v3 回归由
// 域册守(memory_ledger_bridge/subagent integration/recovery 册);v3 原生
// 的综合恢复册随 workflow v3 编排账(V3-GAP-05)接线后另立,不在此处伪造
// 前提。
