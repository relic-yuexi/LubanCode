// ToolDisplay 认 id 不认下标(P4:显示系统剥离单"补稳定 id")。
//
// 钉的是:
//   1. 条目终态按 tool_use_id 路由——两个工具交错(start A, start B, done
//      A, done B)时终态各归各,不串台;
//   2. 迟到的终态(id 已摘除/从未登记)不误伤当前条目;
//   3. 主/子条目同走 id:子工具的 start/result/blocked 认 id;
//   4. 确认块(pending → answered)按 id 定位。
// 旧测试若靠"active_main/active_sub 下标猜当前条目"的行为,这里改测 id。

#include <doctest/doctest.h>

#include <atomic>
#include <vector>

#include "cli/theme.hpp"
#include "cli/tool_display.hpp"
#include "cli/transcript.hpp"
#include "tools/tool.hpp"

namespace cli = lubancode::cli;
using lubancode::tools::Tool;

namespace {

// 管道模式(console=false):条目照进台账,不上屏——单测无终端。
struct DisplayHarness {
    std::vector<cli::TranscriptItem> transcript;
    std::atomic<bool> cancel{false};
    cli::Theme theme;
    cli::ToolDisplay display;

    DisplayHarness() : display(transcript, theme, /*console=*/false, nullptr, &cancel) {}
};

}  // namespace

TEST_CASE("条目终态认 id:两个工具交错,终态各归各") {
    DisplayHarness h;
    h.display.OnToolStart("toolu_A", "read_file", nlohmann::json{{"path", "a.txt"}});
    h.display.OnToolStart("toolu_B", "read_file", nlohmann::json{{"path", "b.txt"}});
    REQUIRE(h.transcript.size() == 2);
    CHECK(h.transcript[0].status == cli::TranscriptStatus::Running);
    CHECK(h.transcript[1].status == cli::TranscriptStatus::Running);

    // B 先完:按 id 落到第二条,第一条仍是 Running。
    h.display.OnToolDone("toolu_B", "read_file", Tool::Result{"内容 B", false});
    CHECK(h.transcript[0].status == cli::TranscriptStatus::Running);
    CHECK(h.transcript[1].status == cli::TranscriptStatus::Ok);

    h.display.OnToolDone("toolu_A", "read_file", Tool::Result{"内容 A", false});
    CHECK(h.transcript[0].status == cli::TranscriptStatus::Ok);
    CHECK(h.transcript[1].status == cli::TranscriptStatus::Ok);
}

TEST_CASE("迟到/陌生的终态不误伤当前条目") {
    DisplayHarness h;
    h.display.OnToolStart("toolu_1", "read_file", nlohmann::json{{"path", "x"}});
    h.display.OnToolDone("toolu_1", "read_file", Tool::Result{"done", false});

    // id 已摘除(toolu_1 终态到过),再来一笔同 id:条目已定格,不再改写。
    h.display.OnToolDone("toolu_1", "read_file", Tool::Result{"late", false});
    CHECK(h.transcript.size() == 1);
    CHECK(h.transcript[0].status == cli::TranscriptStatus::Ok);
    CHECK(h.transcript[0].full_output.find("done") != std::string::npos);
    CHECK(h.transcript[0].full_output.find("late") == std::string::npos);

    // 从未登记的 id:不能把别的进行中条目误收尾。
    h.display.OnToolStart("toolu_2", "read_file", nlohmann::json{{"path", "y"}});
    h.display.OnToolDone("toolu_unknown", "read_file", Tool::Result{"stray", true});
    CHECK(h.transcript[1].status == cli::TranscriptStatus::Running);  // 2 号没被误伤
}

TEST_CASE("主/子条目同走 id:子工具 start/result 按 id 对账") {
    DisplayHarness h;
    h.display.OnToolStart("toolu_agent", "agent", nlohmann::json{{"title", "干活"}});
    h.display.OnSubToolStart("toolu_sub_1", "read_file", nlohmann::json{{"path", "s.txt"}});

    // 子工具按 id 收终态,主 agent 条目不受影响。
    h.display.OnSubToolResult("toolu_sub_1", "read_file", nlohmann::json{},
                              Tool::Result{"子内容", false});
    REQUIRE(h.transcript.size() == 2);
    CHECK(h.transcript[0].kind == cli::TranscriptKind::Tool);
    CHECK(h.transcript[0].status == cli::TranscriptStatus::Running);  // agent 还在跑
    CHECK(h.transcript[1].kind == cli::TranscriptKind::SubTool);
    CHECK(h.transcript[1].status == cli::TranscriptStatus::Ok);

    h.display.OnToolDone("toolu_agent", "agent", Tool::Result{"代理结论", false});
    CHECK(h.transcript[0].status == cli::TranscriptStatus::Ok);
}

TEST_CASE("子工具被钩子拦下:OnSubBlocked 按 id 定格") {
    DisplayHarness h;
    h.display.OnToolStart("toolu_agent", "agent", nlohmann::json{{"title", "干活"}});
    h.display.OnSubToolStart("toolu_sub_9", "run_command", nlohmann::json{{"command", "x"}});
    h.display.OnSubBlocked("toolu_sub_9", "被 PreToolUse 钩子拦截");

    REQUIRE(h.transcript.size() == 2);
    CHECK(h.transcript[1].kind == cli::TranscriptKind::SubTool);
    CHECK(h.transcript[1].status == cli::TranscriptStatus::Error);
    CHECK(h.transcript[0].status == cli::TranscriptStatus::Running);  // 主条目不受牵连
}

TEST_CASE("钩子相位 CheckingHook/Blocked 按 id 路由") {
    DisplayHarness h;
    h.display.OnToolStart("toolu_h1", "run_command", nlohmann::json{{"command", "x"}});
    h.display.OnHookCheckingText("toolu_h1");
    REQUIRE(h.transcript.size() == 1);
    CHECK(h.transcript[0].summary_lines.size() == 1);  // "钩子检查中"文案进摘要

    h.display.OnHookMarkBlocked("toolu_h1");
    CHECK(h.transcript[0].status == cli::TranscriptStatus::Blocked);
}

TEST_CASE("确认块按 id 定位:pending → answered") {
    DisplayHarness h;
    h.display.OnToolStart("toolu_c1", "write_file", nlohmann::json{{"path", "w.txt"}});
    const int pending = h.display.OnConfirmRequest("toolu_c1");
    CHECK(pending >= 0);
    CHECK(h.transcript[0].status == cli::TranscriptStatus::Pending);

    // 拒绝:同一枚 id 收口,条目定格 Cancelled。
    h.display.OnConfirmAnswered(pending, /*allowed=*/false, "toolu_c1");
    CHECK(h.transcript[0].status == cli::TranscriptStatus::Cancelled);

    // 拒绝即终态:同 id 的迟到终态不再改写。
    h.display.OnToolDone("toolu_c1", "write_file", Tool::Result{"用户拒绝执行该工具", true});
    CHECK(h.transcript[0].status == cli::TranscriptStatus::Cancelled);
}

TEST_CASE("确认放行是过渡态:条目回 Running,终态仍等 id") {
    DisplayHarness h;
    h.display.OnToolStart("toolu_c2", "write_file", nlohmann::json{{"path", "w2.txt"}});
    const int pending = h.display.OnConfirmRequest("toolu_c2");
    h.display.OnConfirmAnswered(pending, /*allowed=*/true, "toolu_c2");
    CHECK(h.transcript[0].status == cli::TranscriptStatus::Running);

    h.display.OnToolDone("toolu_c2", "write_file", Tool::Result{"写好了", false});
    CHECK(h.transcript[0].status == cli::TranscriptStatus::Ok);
}
