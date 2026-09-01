// tools::AgentWatchTool(agent_watch,监督器单 P1-0)与台账等待口的单元
// 测试。覆盖单子 §14.1 的 P1 部分:
//   - 入参校验(typed schema 的上限与枚举,execute 侧执法);
//   - 缺省集与 lineage 鉴权:main 看根任务,子代理只看直接孩子,越权稳定拒;
//   - after_revision 命中(变化一到立即醒)/ 超时(无变化睡满 wait,不忙
//     轮询)/ 外部唤醒(NotifyExternalWake)/ 取消旗(父取消经台账 notify);
//   - 输出卫生:events 档不带正文/思考/完整工具参数,diagnostic 只给 main;
//   - 通知按 task_id+health_epoch+reason 去重(恢复成功/用尽/工具结果不明);
//   - session close(单子 §14.2 可本地做的):watcher 与 WaitingChildren 的
//     waiter 都醒,台账有终态。
// 全程假后端 + 手工台账,不碰真网络。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent_progress.hpp"
#include "api/backend.hpp"
#include "tools/agent_tool.hpp"
#include "tools/agent_watch_tool.hpp"
#include "tools/registry.hpp"
#include "tools/task_ledger.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

using Clock = std::chrono::steady_clock;

// 不发一枪的假后端:构造 AgentTool 只要一只 Backend 引用。
class SilentBackend : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(
        const api::Request&, const std::function<void(const api::StreamEvent&)>&,
        const std::atomic<bool>*) override {
        return std::unexpected(api::Error{api::ErrorKind::Api, "SilentBackend", 0});
    }
};

struct Fixture {
    tools::ToolRegistry sub_registry;
    SilentBackend backend;
    std::unique_ptr<tools::AgentTool> agent_tool;
    tools::AgentWatchTool main_watch{nullptr};

    Fixture() {
        agent_tool = std::make_unique<tools::AgentTool>(backend, sub_registry, "/work/dir");
        main_watch = tools::AgentWatchTool(agent_tool.get(), 0);
    }

    tools::TaskLedger& ledger() { return agent_tool->ledger(); }

    // 摆 lineage:#parent(main 直派,depth 1)#child(它的直接孩子,depth 2)
    // #grandchild(#child 的孩子)#sibling(main 直派,与 #parent 同级旁系)。
    struct Lineage {
        std::shared_ptr<tools::TaskRecord> parent;
        std::shared_ptr<tools::TaskRecord> child;
        std::shared_ptr<tools::TaskRecord> grandchild;
        std::shared_ptr<tools::TaskRecord> sibling;
    };

    Lineage MakeLineage() {
        Lineage out;
        tools::AgentTaskSnapshot snap;
        snap.agent_type = "general-purpose";
        snap.title = "母任务";
        snap.prompt = "p";
        snap.start_time = Clock::now();
        out.parent = ledger().Register(snap);

        snap.title = "直接孩子";
        snap.parent_task_id = out.parent->snapshot.id;
        snap.depth = 2;
        snap.root_task_id = out.parent->snapshot.root_task_id;
        out.child = ledger().Register(snap);

        snap.title = "孙任务";
        snap.parent_task_id = out.child->snapshot.id;
        snap.depth = 3;
        out.grandchild = ledger().Register(snap);

        snap.title = "旁系任务";
        snap.parent_task_id = 0;
        snap.depth = 1;
        snap.root_task_id = 0;  // 自己就是根(Register 不重算,手工摆齐)
        out.sibling = ledger().Register(snap);
        return out;
    }
};

nlohmann::json Parse(const tools::Tool::Result& result) {
    return nlohmann::json::parse(result.content);
}

}  // namespace

TEST_CASE("agent_watch:入参校验——typed 上限与枚举在 execute 侧执法") {
    Fixture fx;
    // runtime unavailable。
    tools::AgentWatchTool unavailable(nullptr);
    CHECK(unavailable.execute(nlohmann::json::object()).is_error);

    // 非对象 / 未知键 / 类型错 / 越界。
    CHECK(fx.main_watch.execute(nlohmann::json::array({1})).is_error);
    CHECK(fx.main_watch.execute(nlohmann::json{{"wait_ms", 100}, {"bogus", 1}}).is_error);
    CHECK(fx.main_watch.execute(nlohmann::json{{"task_ids", "not-array"}}).is_error);
    CHECK(fx.main_watch.execute(nlohmann::json{{"task_ids", std::vector<int>(17, 1)}}).is_error);
    CHECK(fx.main_watch.execute(nlohmann::json{{"task_ids", std::vector<std::string>{"1"}}}).is_error);
    CHECK(fx.main_watch.execute(nlohmann::json{{"task_ids", 0}}).is_error);
    CHECK(fx.main_watch.execute(nlohmann::json{{"after_revision", -1}}).is_error);
    CHECK(fx.main_watch.execute(nlohmann::json{{"wait_ms", -5}}).is_error);
    CHECK(fx.main_watch.execute(nlohmann::json{{"wait_ms", "soon"}}).is_error);
    CHECK(fx.main_watch.execute(nlohmann::json{{"include", "gossip"}}).is_error);

    // wait_ms 超 30 秒:钳到上限,不报错。
    const auto clamped = fx.main_watch.execute(nlohmann::json{{"wait_ms", 999999}});
    CHECK_FALSE(clamped.is_error);

    // include=diagnostic 只给 main:子代理窄实例稳定拒绝(不降档冒充)。
    tools::AgentWatchTool scoped(fx.agent_tool.get(), 7);
    CHECK(scoped.execute(nlohmann::json{{"include", "diagnostic"}}).is_error);
    CHECK_FALSE(fx.main_watch.execute(nlohmann::json{{"include", "diagnostic"}}).is_error);
}

TEST_CASE("agent_watch:缺省集按 lineage 折,显式点名越 lineage 稳定拒绝") {
    Fixture fx;
    const Fixture::Lineage ids = fx.MakeLineage();
    tools::AgentWatchTool scoped(fx.agent_tool.get(), ids.parent->snapshot.id);

    // main 缺省:全部未终态根任务(parent + sibling,不含孙)。
    {
        const auto out = Parse(fx.main_watch.execute(nlohmann::json::object()));
        CHECK(out["tasks"].size() == 2);
        bool saw_parent = false;
        bool saw_sibling = false;
        for (const auto& task : out["tasks"]) {
            saw_parent = saw_parent || task["task_id"] == ids.parent->snapshot.id;
            saw_sibling = saw_sibling || task["task_id"] == ids.sibling->snapshot.id;
        }
        CHECK(saw_parent);
        CHECK(saw_sibling);
    }
    // 子代理缺省:只看自己的直接孩子。
    {
        const auto out = Parse(scoped.execute(nlohmann::json::object()));
        REQUIRE(out["tasks"].size() == 1);
        CHECK(out["tasks"][0]["task_id"] == ids.child->snapshot.id);
        CHECK(out["tasks"][0]["state"] == "running");
        CHECK(out["tasks"][0]["health"] == "healthy");
    }
    // 显式点名:main 看孙辈可以(整棵树)……
    CHECK_FALSE(fx.main_watch
                    .execute(nlohmann::json{
                        {"task_ids", std::vector<int>{ids.grandchild->snapshot.id}}})
                    .is_error);
    // ……子代理看孙辈/旁系/自己一律拒绝,拒绝不泄露目标的任何细节。
    for (const int denied :
         {ids.grandchild->snapshot.id, ids.sibling->snapshot.id, ids.parent->snapshot.id}) {
        const auto refused = scoped.execute(nlohmann::json{{"task_ids", std::vector<int>{denied}}});
        CHECK(refused.is_error);
        CHECK(refused.content.find(std::to_string(denied)) != std::string::npos);
    }
    // 不存在的任务号:稳定报 not_found 一类错误。
    CHECK(fx.main_watch.execute(nlohmann::json{{"task_ids", std::vector<int>{999}}}).is_error);

    // 终态任务退出缺省集(main 的根任务收场后不再列)。
    fx.ledger().FinalizeFromToolResult(ids.sibling, "done", false);
    const auto after = Parse(fx.main_watch.execute(nlohmann::json::object()));
    CHECK(after["tasks"].size() == 1);
}

TEST_CASE("agent_watch:after_revision 命中——变化一到立即醒,不吃满等待") {
    Fixture fx;
    const Fixture::Lineage ids = fx.MakeLineage();
    const std::uint64_t revision = fx.ledger().watch_generation();

    // 50ms 后推进实质进展:watcher 应远早于 2000ms 的 wait 上限醒来。
    std::thread changer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        fx.ledger().RecordAssistantMessage(ids.sibling,
                                           agent::FingerprintOfParts("msg", std::to_string(Clock::now().time_since_epoch().count())));
    });
    const auto started = Clock::now();
    const auto out = Parse(fx.main_watch.execute(
        nlohmann::json{{"after_revision", revision}, {"wait_ms", 2000}}));
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    changer.join();
    CHECK(out["timed_out"] == false);
    CHECK(out["revision"].get<std::uint64_t>() > revision);
    CHECK(elapsed_ms < 1500);  // 睡在 cv 上,变化一到就醒(宽裕的 CI 余量)
    // 快照照实反映新账:进展龄有了,不再是 null。
    bool saw_progress_age = false;
    for (const auto& task : out["tasks"]) {
        if (task["task_id"] == ids.sibling->snapshot.id) {
            saw_progress_age = task.contains("last_progress_age_ms") && !task["last_progress_age_ms"].is_null();
        }
    }
    CHECK(saw_progress_age);
}

TEST_CASE("agent_watch:超时——无变化睡满 wait 才回,cv 上零忙轮询") {
    Fixture fx;
    fx.MakeLineage();
    const std::uint64_t revision = fx.ledger().watch_generation();
    const auto started = Clock::now();
    const auto out = Parse(fx.main_watch.execute(
        nlohmann::json{{"after_revision", revision}, {"wait_ms", 250}}));
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    CHECK(out["timed_out"] == true);
    CHECK(out["revision"].get<std::uint64_t>() == revision);
    CHECK(elapsed_ms >= 240);  // 没变化不许提前回(忙轮询/虚醒误报都在这拦)
}

TEST_CASE("agent_watch:外部唤醒与取消旗提前叫醒等待") {
    Fixture fx;
    fx.MakeLineage();
    const std::uint64_t revision = fx.ledger().watch_generation();

    // 外部唤醒(ESC/用户排队那一路的 NotifyExternalWake)。
    std::thread waker([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        fx.ledger().NotifyExternalWake();
    });
    {
        const auto started = Clock::now();
        const auto out = Parse(fx.main_watch.execute(
            nlohmann::json{{"after_revision", revision}, {"wait_ms", 5000}}));
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        CHECK(out["timed_out"] == false);
        CHECK(elapsed_ms < 1500);
    }
    waker.join();

    // 取消旗:父取消经 CancelTask 一路的台账 notify 叫醒,谓词再查旗。
    std::atomic<bool> cancel_flag{false};
    tools::ToolExecutionContext context;
    context.cancel = &cancel_flag;
    const std::uint64_t revision_after_wake = fx.ledger().watch_generation();
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        cancel_flag.store(true);
        // 子代理等孩子时,父取消它 = CancelTask(cancel 树)——那条路在台账
        // 锁内 notify_all,watcher 因此醒来并看见旗。
        fx.ledger().BroadcastCancel();
    });
    const auto started = Clock::now();
    const auto out = Parse(fx.main_watch.execute(
        nlohmann::json{{"after_revision", revision_after_wake}, {"wait_ms", 8000}}, context));
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    canceller.join();
    CHECK(elapsed_ms < 2000);  // 取消压过等待(单子不变量 10)
    (void)out;
}

TEST_CASE("agent_watch:输出卫生——events 不带正文/参数,diagnostic 只给计数与码") {
    Fixture fx;
    const Fixture::Lineage ids = fx.MakeLineage();
    // 摆两笔带"敏感内容"的事件:正文与完整工具参数。
    {
        std::lock_guard<std::mutex> lock(fx.ledger().mutex);
        tools::AgentTaskEvent secret_text;
        secret_text.kind = tools::AgentTaskEventKind::AssistantText;
        secret_text.text = "机密正文不该出去";
        fx.ledger().AppendEventLocked(ids.sibling, std::move(secret_text));
        tools::AgentTaskEvent secret_tool;
        secret_tool.kind = tools::AgentTaskEventKind::ToolStart;
        secret_tool.tool_name = "write_file";
        secret_tool.input_json = R"({"path":"C:/secret","content":"SECRET"})";
        fx.ledger().AppendEventLocked(ids.sibling, std::move(secret_tool));
    }
    const auto out = Parse(fx.main_watch.execute(
        nlohmann::json{{"task_ids", std::vector<int>{ids.sibling->snapshot.id}},
                       {"include", "events"},
                       {"after_revision", 0}}));
    REQUIRE(out["tasks"].size() == 1);
    const auto& task = out["tasks"][0];
    REQUIRE(task.contains("events"));
    CHECK(task["events"].size() >= 2);
    const std::string dumped = task.dump();
    CHECK(dumped.find("机密正文") == std::string::npos);
    CHECK(dumped.find("SECRET") == std::string::npos);
    CHECK(dumped.find("write_file") != std::string::npos);  // 工具名是稳定码,可以给

    // 事件过滤:after_revision 取上一轮的事件号,只回其后的事件。
    std::uint64_t last_revision = 0;
    for (const auto& event : task["events"]) {
        last_revision = std::max(last_revision, event["revision"].get<std::uint64_t>());
    }
    {
        std::lock_guard<std::mutex> lock(fx.ledger().mutex);
        tools::AgentTaskEvent newer;
        newer.kind = tools::AgentTaskEventKind::ToolResult;
        newer.tool_name = "read_file";
        fx.ledger().AppendEventLocked(ids.sibling, std::move(newer));
    }
    const auto delta = Parse(fx.main_watch.execute(
        nlohmann::json{{"task_ids", std::vector<int>{ids.sibling->snapshot.id}},
                       {"include", "events"},
                       {"after_revision", last_revision}}));
    REQUIRE(delta["tasks"].size() == 1);
    REQUIRE(delta["tasks"][0]["events"].size() == 1);
    CHECK(delta["tasks"][0]["events"][0]["tool"] == "read_file");
    CHECK(delta["tasks"][0]["events"][0]["revision"].get<std::uint64_t>() > last_revision);

    // diagnostic(main):计数与稳定码,无正文;墙钟剩余按预算折。
    const auto diag = Parse(fx.main_watch.execute(
        nlohmann::json{{"task_ids", std::vector<int>{ids.sibling->snapshot.id}},
                       {"include", "diagnostic"}}));
    REQUIRE(diag["tasks"].size() == 1);
    CHECK(diag["tasks"][0].contains("diagnostic"));
    CHECK(diag["tasks"][0]["diagnostic"].contains("health_epoch"));
    CHECK(diag["tasks"][0]["diagnostic"].contains("retry_count"));
    CHECK(diag.dump().find("机密正文") == std::string::npos);
}

TEST_CASE("通知去重:恢复成功/用尽/工具结果不明按 task+epoch+reason 只弹一次") {
    Fixture fx;
    const auto task = fx.MakeLineage().sibling;

    // 恢复成功:一次恢复 episode 恰一条;重试账先立(Retrying)再收口。
    fx.ledger().RecordRequestRetry(task, 2, "network.error");
    fx.ledger().RecordRequestOutcome(task, true, std::string());
    fx.ledger().RecordRequestOutcome(task, true, std::string());  // 同代际不重弹
    const auto recovered = fx.ledger().TakeSupervisorNotices();
    REQUIRE(recovered.size() == 1);
    CHECK(recovered[0].find("重连成功") != std::string::npos);

    // 用尽:同因同代际只一条;用户取消(cancelled)不弹"用尽"。
    fx.ledger().RecordRecoveryExhausted(task, "http.503");
    fx.ledger().RecordRecoveryExhausted(task, "http.503");
    fx.ledger().RecordRecoveryExhausted(task, "cancelled");
    const auto exhausted = fx.ledger().TakeSupervisorNotices();
    REQUIRE(exhausted.size() == 1);
    CHECK(exhausted[0].find("用尽") != std::string::npos);

    // 工具结果不明:同一笔调用只一条(键带 tool_use_id),账面留"结果不明"卡。
    {
        std::lock_guard<std::mutex> lock(fx.ledger().mutex);
        task->snapshot.tool_calls.push_back(
            tools::AgentTaskToolCall{"run_command", "{}", std::string(), false, false, "tu-1"});
        fx.ledger().RecordToolIndeterminateLocked(task, "run_command", "tu-1");
        fx.ledger().RecordToolIndeterminateLocked(task, "run_command", "tu-1");  // 第二笔不弹
    }
    const auto indeterminate = fx.ledger().TakeSupervisorNotices();
    REQUIRE(indeterminate.size() == 1);
    CHECK(indeterminate[0].find("结果不明") != std::string::npos);
    const auto detail = fx.ledger().Detail(task->snapshot.id);
    REQUIRE(detail.has_value());
    REQUIRE(detail->tool_calls.size() == 1);
    CHECK(detail->tool_calls[0].done);
    CHECK(detail->tool_calls[0].is_error);
    CHECK(detail->tool_calls[0].result.find("结果不明") != std::string::npos);
}

TEST_CASE("session close:watcher 与 waiter 都醒,台账有终态(单子 §14.2 本地件)") {
    Fixture fx;
    const Fixture::Lineage ids = fx.MakeLineage();
    // 母任务进 WaitingChildren 等孩子(WaitForKeyChange 的 waiter)。
    fx.ledger().SetLiveTaskState(ids.parent, tools::AgentTaskState::WaitingChildren);
    std::atomic<bool> waiter_woke{false};
    std::thread waiter([&] {
        fx.ledger().WaitForKeyChange(ids.parent);
        waiter_woke.store(true);
    });
    // watcher:main 等根任务变化。
    std::atomic<bool> watcher_done{false};
    const std::uint64_t revision = fx.ledger().watch_generation();
    std::thread watcher([&] {
        const auto out = Parse(fx.main_watch.execute(
            nlohmann::json{{"after_revision", revision}, {"wait_ms", 10000}}));
        CHECK(out["timed_out"] == false);  // session close 提前唤醒,不吃满 10s
        watcher_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // session close:广播取消(watcher 醒),全部任务先后收账(终态)。
    fx.ledger().BroadcastCancel();
    fx.ledger().FinalizeFromToolResult(ids.child, "被取消", true);
    fx.ledger().FinalizeFromToolResult(ids.parent, "被取消", true);
    fx.ledger().FinalizeFromToolResult(ids.grandchild, "被取消", true);
    fx.ledger().FinalizeFromToolResult(ids.sibling, "被取消", true);
    watcher.join();
    waiter.join();
    CHECK(watcher_done.load());
    CHECK(waiter_woke.load());
    const auto summaries = fx.ledger().Summaries();
    REQUIRE(summaries.size() == 4);
    for (const auto& summary : summaries) {
        CHECK_FALSE(tools::IsAliveTaskState(summary.state));  // 台账有终态
    }
    CHECK(fx.ledger().ProgressOf(ids.parent->snapshot.id).health == agent::AgentHealthState::Terminal);
    CHECK(fx.ledger().ProgressOf(ids.child->snapshot.id).health == agent::AgentHealthState::Terminal);
}

// LocalVariables:
// fill-column: 100
// End:
