// workflow 会话归属统一单:TrajectorySessionLedger 编排账(SpawnWorkflowRun/
// SpawnNodeStream 即 ReserveWorkflowRun/ReserveWorkflowNodeStream 的消费方)。
// V3-LEGACY-01(2026-09-24):v2 父场随写口退役造不出,编排 1-5/7 的 ledger
// 路整段退役(见册内注);编排 6(旧 workflow-runs/ 路兼容)不经建场口,保留。
// SpawnNodeStream 即 ReserveWorkflowRun/ReserveWorkflowNodeStream 的消费方)。
// 六场:
//   1. reserve→consume 全链:编排 Journal 只见编排事实;node 账收模型/工具
//      事件(ownership:声明才进、无主拒);verify 全过且父子边逐位对账。
//   2. 无主 tool trace:node 桥不造册不落账(与子代理同门)。
//   3. node 账开张失败:fail closed,无 0 字节残留,verify 过。
//   4. 编排账开张失败:fail closed,无 stream,verify 过。
//   5. retry 新开 node 文件:attempt 1 与 attempt 2 不共写。
//   6. 旧 workflow-runs/ 路兼容:照写照读,不迁移不炸。
//   7. hash 对账负例:编排记了假 hash,verify 报 edge.child_hash_mismatch。
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/replay.hpp"
#include "workflow/journal.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

std::filesystem::path FreshDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// (V3-LEGACY-01:编排 1-5/7 的 helper 随案退役删除;FreshDir 留给编排 6)

}  // namespace

// (退役,V3-LEGACY-01)原此处有"编排 1/2/3/4/5"等 5 案:经 TrajectorySessionLedger
// 开 v2 父场(env 注入 0)再 SpawnWorkflowRun,验旧 v2 编排桥的 reserve/
// consume/fail-closed/retry/hash 对账全链。写口退役后 v2 父场造不出
// (SpawnWorkflowRun 对 v3 活场在 trajectory_workflow_bridge.cpp:510 解引用
// 空 main,是存量隐患,见销项册 V3-LEGACY-01 边界注);workflow 的 v3
// 编排账(account_root/WorkflowRunAccount)接线归 V3-GAP-05,旧桥回归随
// 旧盘 v2 活场(恢复收养)另立夹具单守。以下整段退役,不硬凑前提。

TEST_CASE("编排 6:旧 workflow-runs/ 路兼容——照写照读,不迁移不炸") {
    const auto root = FreshDir("lubancode-traj-wf-legacy");
    workflow::RunJournal::StartInfo info;
    info.run_id = "run-legacy-1";
    info.workflow_id = "probe-flow";
    info.workflow_version = "1.0.0";
    info.content_hash = std::string(64, '9');
    info.cwd = "D:/repo";
    info.definition_json = R"({"id":"probe-flow"})";
    auto journal = workflow::RunJournal::Start(root / "workflow-runs", info);
    REQUIRE(journal.has_value());
    journal->Append(workflow::kEventRunStarted, "", 0, nlohmann::json{{"state", "running"}});
    journal->Append(workflow::kEventNodeCompleted, "x", 1,
                    nlohmann::json{{"outcome", "success"}});
    journal->Finish("succeeded", nlohmann::json{{"tokens", 3}});

    // 旧读口全绿:ListRuns 排得出、事件读得回(Start/Finish 自带 run 边界
    // 事件,按内容断言,不数数)。
    const auto runs = workflow::ListRuns(root / "workflow-runs");
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].run_id == "run-legacy-1");
    CHECK(runs[0].final_state == "succeeded");
    const auto events = workflow::ReadJournalEvents(runs[0].dir);
    REQUIRE(events.size() >= 3);
    CHECK(events.front().type == workflow::kEventRunStarted);
    CHECK(events.back().type == workflow::kEventRunCompleted);
    bool saw_node = false;
    for (const auto& event : events) {
        if (event.type == workflow::kEventNodeCompleted && event.node_id == "x") {
            saw_node = true;
        }
    }
    CHECK(saw_node);
}

// (退役,V3-LEGACY-01)原此处有"编排 7"等 1 案:经 TrajectorySessionLedger
// 开 v2 父场(env 注入 0)再 SpawnWorkflowRun,验旧 v2 编排桥的 reserve/
// consume/fail-closed/retry/hash 对账全链。写口退役后 v2 父场造不出
// (SpawnWorkflowRun 对 v3 活场在 trajectory_workflow_bridge.cpp:510 解引用
// 空 main,是存量隐患,见销项册 V3-LEGACY-01 边界注);workflow 的 v3
// 编排账(account_root/WorkflowRunAccount)接线归 V3-GAP-05,旧桥回归随
// 旧盘 v2 活场(恢复收养)另立夹具单守。以下整段退役,不硬凑前提。
