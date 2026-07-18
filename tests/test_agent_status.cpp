// #52:子代理状态条——AgentStatusBoard 的状态流转(Start/RecordToolCall/
// Finish/Clear/Snapshot/HasRunning/Empty)+ FormatAgentStatusLines 的纯
// 渲染逻辑(彩色/plain、Running 用 now 现算耗时、Ok/Error 用定住的
// end_time)。

#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <vector>

#include "cli/agent_status.hpp"
#include "cli/theme.hpp"

using lubancode::cli::AgentStatusBoard;
using lubancode::cli::AgentStatusEntry;
using lubancode::cli::AgentStatusState;
using lubancode::cli::BuiltinTheme;
using lubancode::cli::FormatAgentStatusLines;

namespace {
constexpr const char* kDot = "\xE2\x97\x8F";  // ●
}  // namespace

// ---- AgentStatusBoard:状态流转 --------------------------------------------

TEST_CASE("AgentStatusBoard: Start 起一条 Running 态,id 从 1 开始递增") {
    AgentStatusBoard board;
    const int id1 = board.Start("翻找并总结 src/ 里的日志代码");
    const int id2 = board.Start("第二个任务");
    CHECK(id1 == 1);
    CHECK(id2 == 2);

    const auto snap = board.Snapshot();
    REQUIRE(snap.size() == 2);
    CHECK(snap[0].id == id1);
    CHECK(snap[0].state == AgentStatusState::Running);
    CHECK(snap[0].tool_calls == 0);
    CHECK(snap[1].id == id2);
}

TEST_CASE("AgentStatusBoard: label 超过 40 码点截断,换行/制表符压成空格") {
    AgentStatusBoard board;
    const std::string long_label(50, 'a');
    const int id = board.Start(long_label);
    const auto snap = board.Snapshot();
    REQUIRE(snap.size() == 1);
    CHECK(snap[0].label.size() < long_label.size());
    CHECK(snap[0].label.find("...") != std::string::npos);

    AgentStatusBoard board2;
    board2.Start("第一行\n第二行\t带制表符");
    const auto snap2 = board2.Snapshot();
    CHECK(snap2[0].label.find('\n') == std::string::npos);
    CHECK(snap2[0].label.find('\t') == std::string::npos);
    (void)id;
}

TEST_CASE("AgentStatusBoard: RecordToolCall 累加计数,Finish 收成终态") {
    AgentStatusBoard board;
    const int id = board.Start("任务");
    board.RecordToolCall(id);
    board.RecordToolCall(id);
    board.RecordToolCall(id);
    board.Finish(id, /*success=*/true);

    const auto snap = board.Snapshot();
    REQUIRE(snap.size() == 1);
    CHECK(snap[0].tool_calls == 3);
    CHECK(snap[0].state == AgentStatusState::Ok);

    // 收尾之后再调用一律不生效——已经定型的条目不该被晚到的事件改动。
    board.RecordToolCall(id);
    board.Finish(id, /*success=*/false);
    const auto snap2 = board.Snapshot();
    CHECK(snap2[0].tool_calls == 3);
    CHECK(snap2[0].state == AgentStatusState::Ok);
}

TEST_CASE("AgentStatusBoard: Finish(false) 收成 Error 态") {
    AgentStatusBoard board;
    const int id = board.Start("任务");
    board.Finish(id, /*success=*/false);
    const auto snap = board.Snapshot();
    REQUIRE(snap.size() == 1);
    CHECK(snap[0].state == AgentStatusState::Error);
}

TEST_CASE("AgentStatusBoard: 认不出的 id(晚到/野生事件)安静地什么也不做") {
    AgentStatusBoard board;
    const int id = board.Start("任务");
    board.RecordToolCall(id + 100);
    board.Finish(id + 100, true);
    const auto snap = board.Snapshot();
    REQUIRE(snap.size() == 1);
    CHECK(snap[0].tool_calls == 0);
    CHECK(snap[0].state == AgentStatusState::Running);
}

TEST_CASE("AgentStatusBoard: HasRunning / Empty / Clear") {
    AgentStatusBoard board;
    CHECK(board.Empty());
    CHECK_FALSE(board.HasRunning());

    const int id1 = board.Start("任务一");
    CHECK_FALSE(board.Empty());
    CHECK(board.HasRunning());

    board.Finish(id1, true);
    CHECK_FALSE(board.HasRunning());  // 全部收尾,没有 Running 态了
    CHECK_FALSE(board.Empty());       // 但收尾行还在,列表没空

    board.Clear();
    CHECK(board.Empty());
    CHECK_FALSE(board.HasRunning());

    // Clear 之后 id 从 1 重新计——新一轮 RunTurn 干净起步。
    const int id2 = board.Start("下一轮的任务");
    CHECK(id2 == 1);
}

// ---- FormatAgentStatusLines:纯渲染 ----------------------------------------

TEST_CASE("FormatAgentStatusLines: plain 主题下三态各自的文字状态词,不夹 ANSI/圆点") {
    const auto theme = BuiltinTheme("plain");
    const auto now = std::chrono::steady_clock::now();

    AgentStatusEntry running;
    running.label = "跑着的任务";
    running.state = AgentStatusState::Running;
    running.tool_calls = 2;
    running.start_time = now - std::chrono::seconds(5);

    AgentStatusEntry ok;
    ok.label = "成功的任务";
    ok.state = AgentStatusState::Ok;
    ok.tool_calls = 4;
    ok.start_time = now - std::chrono::seconds(10);
    ok.end_time = now - std::chrono::seconds(3);  // 定住的耗时:7s,不受 now 影响

    AgentStatusEntry error;
    error.label = "失败的任务";
    error.state = AgentStatusState::Error;
    error.tool_calls = 1;
    error.start_time = now - std::chrono::seconds(2);
    error.end_time = now - std::chrono::milliseconds(500);

    const auto lines = FormatAgentStatusLines({running, ok, error}, now, theme);
    REQUIRE(lines.size() == 3);

    CHECK(lines[0].find("[RUNNING]") == 0);
    CHECK(lines[0].find("跑着的任务") != std::string::npos);
    CHECK(lines[0].find("2") != std::string::npos);  // 工具调用次数
    CHECK(lines[0].find(kDot) == std::string::npos);
    CHECK(lines[0].find("\x1b") == std::string::npos);

    CHECK(lines[1].find("[OK]") == 0);
    CHECK(lines[1].find("成功的任务") != std::string::npos);
    CHECK(lines[1].find("7.0s") != std::string::npos);  // end_time - start_time,不随 now 变

    CHECK(lines[2].find("[ERROR]") == 0);
    CHECK(lines[2].find("失败的任务") != std::string::npos);
}

TEST_CASE("FormatAgentStatusLines: 彩色主题只染状态灯(●),文字不染色") {
    const auto theme = BuiltinTheme("dark");
    const auto now = std::chrono::steady_clock::now();

    AgentStatusEntry running;
    running.label = "任务";
    running.state = AgentStatusState::Running;
    running.start_time = now;

    const auto lines = FormatAgentStatusLines({running}, now, theme);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find(kDot) != std::string::npos);
    CHECK(lines[0].find("[RUNNING]") == std::string::npos);
    CHECK(lines[0].find(theme.reset) != std::string::npos);
}

TEST_CASE("FormatAgentStatusLines: 空列表给空列表,不崩不占行") {
    const auto theme = BuiltinTheme("dark");
    const auto lines = FormatAgentStatusLines({}, std::chrono::steady_clock::now(), theme);
    CHECK(lines.empty());
}

TEST_CASE("FormatAgentStatusLines: Running 态耗时随 now 走,Ok 态耗时定住不随 now 走") {
    const auto theme = BuiltinTheme("plain");
    const auto start = std::chrono::steady_clock::now();

    AgentStatusEntry running;
    running.label = "任务";
    running.state = AgentStatusState::Running;
    running.start_time = start;

    const auto line_at_1s = FormatAgentStatusLines({running}, start + std::chrono::seconds(1), theme);
    const auto line_at_5s = FormatAgentStatusLines({running}, start + std::chrono::seconds(5), theme);
    CHECK(line_at_1s[0].find("1.0s") != std::string::npos);
    CHECK(line_at_5s[0].find("5.0s") != std::string::npos);
    CHECK(line_at_1s[0] != line_at_5s[0]);
}
