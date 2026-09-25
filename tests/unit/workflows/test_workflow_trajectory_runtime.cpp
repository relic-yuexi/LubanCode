// workflow 会话归属统一单:WorkflowRuntime × 真 TrajectorySessionLedger 的
// 编排账集成(fail closed 合同由 runtime 落)。四场:
//   1. 顺序图:编排账 + 每节点一份 node 账,trajectory verify 全过。
//   2. parallel + map:并发各路 node stream 互不串线,verify 全过。
//   3. loop + retry:重试新开 node 文件、loop 重入新派发号,verify 全过。
//   4. fail closed:编排账/node 账开不出,run/节点停在明确失败态,verify 过。
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/replay.hpp"
#include "workflow/parser.hpp"
#include "workflow/runtime.hpp"

// (V3-LEGACY-01:原匿名命名空间内的五案 helper 随案一并退役删除)

// (退役,V3-LEGACY-01)原此处有"runtime 集成 1/2/3/4/5"等 5 案:经 TrajectorySessionLedger
// 开 v2 父场(env 注入 0)再 SpawnWorkflowRun,验旧 v2 编排桥的 reserve/
// consume/fail-closed/retry/hash 对账全链。写口退役后 v2 父场造不出
// (SpawnWorkflowRun 对 v3 活场在 trajectory_workflow_bridge.cpp:510 解引用
// 空 main,是存量隐患,见销项册 V3-LEGACY-01 边界注);workflow 的 v3
// 编排账(account_root/WorkflowRunAccount)接线归 V3-GAP-05,旧桥回归随
// 旧盘 v2 活场(恢复收养)另立夹具单守。以下整段退役,不硬凑前提。
