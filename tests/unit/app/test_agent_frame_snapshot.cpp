// 按代理状态投影单 P1:统一快照合同(AgentFrameSnapshot)的适配回归册。
//
// main 复用 TurnCollector/TurnView,sub 复用任务事件(AgentTaskEvent)——
// 两路适配成同一份只读快照(单子 §四.2):稳定 item ID、内容 revision、
// 活动与统计 revision。P1 先立合同钉单测;统一 renderer(P2)接手后
// 消费的就是这份形状。

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "app/agent_panel_presenter.hpp"
#include "cli/agent_view_state.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/turn_collector.hpp"
#include "tools/task_ledger.hpp"

using namespace lubancode;

namespace {

tools::AgentTaskEvent MakeTaskEvent(tools::AgentTaskEventKind kind) {
    tools::AgentTaskEvent event;
    event.kind = kind;
    return event;
}

}  // namespace

TEST_CASE("P1 快照: main 侧适配——TurnView 原样进合同,修订号成对") {
    runtime::TurnCollector collector(runtime::ProcessIdAuthority(), "turn-snap");
    collector.StartTurn("用户问题", 1);
    collector.OnTextDelta("回答正文", false);
    collector.OnModelStepStarted(0);

    const cli::AgentViewKey key{7, 0};
    auto view = std::make_shared<const runtime::TurnView>(collector.view());
    const cli::AgentFrameSnapshot snapshot =
        cli::AgentFrameSnapshotFromTurnView(key, view, /*content=*/12, /*activity=*/3, /*stats=*/5,
                                            /*running=*/true);
    CHECK(snapshot.key == key);
    CHECK(snapshot.key.is_main());
    CHECK(snapshot.content_revision == 12);
    CHECK(snapshot.activity_revision == 3);
    CHECK(snapshot.stats_revision == 5);
    CHECK(snapshot.running);
    REQUIRE(snapshot.turn != nullptr);
    // 条目带稳定 id(IdAuthority 发的 item-<n>)、user 条目在账。
    CHECK_FALSE(snapshot.turn->items.empty());
    CHECK_FALSE(snapshot.turn->user_item_id.empty());
    bool has_text = false;
    for (const auto& item : snapshot.turn->items) {
        has_text = has_text || item.kind == runtime::TurnItemViewKind::Text;
        CHECK_FALSE(item.item_id.empty());
    }
    CHECK(has_text);
}

TEST_CASE("P1 快照: sub 侧适配——事件账折成同一份合同,工具按 id 配对") {
    const cli::AgentViewKey key{7, 4};
    std::vector<tools::AgentTaskEvent> events;

    tools::AgentTaskEvent user = MakeTaskEvent(tools::AgentTaskEventKind::UserMessage);
    user.text = "去查目录";
    user.item_id = "item-1";
    events.push_back(user);

    tools::AgentTaskEvent reasoning = MakeTaskEvent(tools::AgentTaskEventKind::AssistantReasoning);
    reasoning.text = "先想想";
    reasoning.streaming = true;
    reasoning.item_id = "item-2";
    events.push_back(reasoning);

    tools::AgentTaskEvent tool_start = MakeTaskEvent(tools::AgentTaskEventKind::ToolStart);
    tool_start.tool_name = "read_file";
    tool_start.tool_use_id = "toolu_A";
    tool_start.input_json = R"({"path":"a.txt"})";
    tool_start.item_id = "item-3";
    events.push_back(tool_start);

    tools::AgentTaskEvent text = MakeTaskEvent(tools::AgentTaskEventKind::AssistantText);
    text.text = "中间正文";
    text.item_id = "item-4";
    events.push_back(text);

    tools::AgentTaskEvent tool_done = MakeTaskEvent(tools::AgentTaskEventKind::ToolResult);
    tool_done.tool_name = "read_file";
    tool_done.tool_use_id = "toolu_A";
    tool_done.result = "文件内容";
    tool_done.tool_status = tools::AgentTaskToolStatus::Succeeded;
    tool_done.item_id = "item-5";
    events.push_back(tool_done);

    tools::AgentTaskEvent done = MakeTaskEvent(tools::AgentTaskEventKind::Completion);
    done.text = "查完了";
    done.item_id = "item-6";
    events.push_back(done);

    const cli::AgentFrameSnapshot snapshot =
        app::BuildAgentFrameSnapshotFromTaskEvents(key, events, /*running=*/false, "审查目录", 33, 9);
    CHECK(snapshot.key == key);
    CHECK_FALSE(snapshot.running);
    CHECK(snapshot.title == "审查目录");
    CHECK(snapshot.content_revision == 33);
    CHECK(snapshot.stats_revision == 9);
    REQUIRE(snapshot.turn != nullptr);

    // 稳定 item ID:事件带 item_id 的原样用;条目按事件序,工具卡落在
    // start 位置(终态并入),ToolResult 不另占一枚。
    std::vector<std::string> ids;
    for (const auto& item : snapshot.turn->items) {
        ids.push_back(item.item_id);
    }
    REQUIRE(ids.size() == 5);  // user/thinking/tool(start 吃终态)/text/completion
    CHECK(ids[0] == "item-1");
    CHECK(ids[1] == "item-2");
    CHECK(ids[2] == "item-3");
    CHECK(ids[3] == "item-4");
    CHECK(ids[4] == "item-6");

    // 工具配对:start 那枚吃进终态,同 id 的 result 不另开卡。
    const runtime::TurnItemView& tool_item = snapshot.turn->items[2];
    CHECK(tool_item.kind == runtime::TurnItemViewKind::Tool);
    CHECK(tool_item.tool_use_id == "toolu_A");
    CHECK(tool_item.status == runtime::TurnItemViewState::Succeeded);
    CHECK(tool_item.result_text == "文件内容");
    CHECK(snapshot.turn->metrics.tool_count == 1);
    CHECK(snapshot.turn->status == runtime::TurnItemViewState::Succeeded);

    // 种类映射:思考/正文/收尾各归各位。
    CHECK(snapshot.turn->items[1].kind == runtime::TurnItemViewKind::Thinking);
    CHECK(snapshot.turn->items[3].kind == runtime::TurnItemViewKind::Text);
    CHECK(snapshot.turn->items[4].kind == runtime::TurnItemViewKind::Text);
}

TEST_CASE("P1 快照: sub 侧适配——运行中任务、失败工具与孤儿 result 各安其位") {
    const cli::AgentViewKey key{9, 11};
    std::vector<tools::AgentTaskEvent> events;

    tools::AgentTaskEvent tool_start = MakeTaskEvent(tools::AgentTaskEventKind::ToolStart);
    tool_start.tool_name = "run_command";
    tool_start.tool_use_id = "toolu_B";
    tool_start.input_json = "";
    events.push_back(tool_start);

    tools::AgentTaskEvent tool_fail = MakeTaskEvent(tools::AgentTaskEventKind::ToolResult);
    tool_fail.tool_name = "run_command";
    tool_fail.tool_use_id = "toolu_B";
    tool_fail.result = "exit 1";
    tool_fail.is_error = true;
    tool_fail.tool_status = tools::AgentTaskToolStatus::Interrupted;
    events.push_back(tool_fail);

    tools::AgentTaskEvent orphan = MakeTaskEvent(tools::AgentTaskEventKind::ToolResult);
    orphan.tool_name = "web_fetch";
    orphan.tool_use_id = "toolu_LATE";
    orphan.result = "迟到的结果";
    events.push_back(orphan);

    const cli::AgentFrameSnapshot snapshot =
        app::BuildAgentFrameSnapshotFromTaskEvents(key, events, /*running=*/true, std::string(), 4, 1);
    CHECK(snapshot.running);
    CHECK(snapshot.turn->status == runtime::TurnItemViewState::Running);
    REQUIRE(snapshot.turn->items.size() == 2);  // start(吃终态) + 孤儿卡
    CHECK(snapshot.turn->items[0].status == runtime::TurnItemViewState::Interrupted);
    CHECK(snapshot.turn->items[0].result_is_error);
    CHECK(snapshot.turn->items[1].kind == runtime::TurnItemViewKind::Tool);
    CHECK(snapshot.turn->items[1].result_text == "迟到的结果");
    // 旧账没有 item_id:合成号兜底,不空。
    CHECK_FALSE(snapshot.turn->items[0].item_id.empty());
    CHECK(snapshot.turn->items[0].item_id.find("task-11-") == 0);
}
