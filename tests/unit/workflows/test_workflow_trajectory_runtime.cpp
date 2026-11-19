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

// Actual V3 recovery lives here; the retired v2 write bridge stays retired.
#include "workflow_account_runtime_fixture.hpp"

TEST_CASE("账路恢复:预算计数不归零,token 账跨恢复延续") {
    const fs::path root = TempRoot("budget");
    auto parsed = ParseWorkflowYaml(kLinearYaml);
    REQUIRE(parsed.has_value());

    auto first_executor = std::make_shared<PerNodeExecutor>();
    first_executor->script["a"] = {{true, "", nlohmann::json{{"who", std::string("a")}}, 7}};
    first_executor->script["b"] = {{true, "", nlohmann::json{{"who", std::string("b")}}, 3}};
    RuntimeOptions first_options = BaseOptions(root, first_executor);
    int writes = 0;
    first_options.account_fault = [&writes] {
        ++writes;
        // a/b 全部收口(#1-#10 落稳),终态前 checkpoint(#11)注入失败:
        // 执行事实齐全、run 无终态——恢复只补收口,零新执行。
        return writes == 11 ? std::optional<std::string>("test.injected") : std::nullopt;
    };
    WorkflowRuntime first(first_options);
    const auto interrupted = first.Run(*parsed, RunInputs{});
    // 执行全成、终态没写住:summary 如实带 account_terminal_unwritten。
    REQUIRE(interrupted.state == RunState::Succeeded);
    CHECK(interrupted.error_code == "account_terminal_unwritten");
    CHECK(interrupted.tokens_used == 10);

    auto second_executor = std::make_shared<PerNodeExecutor>();
    WorkflowRuntime resumer(BaseOptions(root, second_executor));
    const auto resumed = resumer.Resume(root / "run-acc");
    REQUIRE(resumed.has_value());
    CHECK(resumed->state == RunState::Succeeded);
    CHECK(second_executor->calls.empty());  // 事实齐全:恢复零新执行
    // 计数不归零:两只节点的 token 账(7+3)原样带进恢复后的 run。
    CHECK(resumed->tokens_used == 10);
    // 终态这次写住了:再 resume 拒绝复活。
    CHECK_FALSE(resumer.Resume(root / "run-acc").has_value());
}
