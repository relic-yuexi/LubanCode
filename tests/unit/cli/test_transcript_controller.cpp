// transcript 控制器(终端接线收尾单)的合同测试:焦点导航、查看态进出、
// 轮次导航、空账本口径。输出经 TerminalPort 捕获,不污染测试台。

#include <chrono>
#include <sstream>
#include <vector>

#include <doctest/doctest.h>

#include "api/types.hpp"
#include "cli/console_input.hpp"  // PanelSessionSlot/CurrentAgentViewedTaskId(查看态 Esc 兜底测试)
#include "cli/terminal_port.hpp"
#include "cli/transcript_controller.hpp"

namespace {

lubancode::cli::TranscriptUiController MakeController(std::ostringstream& out_capture,
                                                       const lubancode::cli::Theme& theme) {
    lubancode::cli::TermPort().Redirect(&out_capture, nullptr);
    return lubancode::cli::TranscriptUiController{theme};
}

std::vector<lubancode::api::Message> TwoTurnHistory() {
    using lubancode::api::Message;
    using lubancode::api::Role;
    using lubancode::api::TextBlock;
    std::vector<Message> history;
    Message first;
    first.role = Role::User;
    first.content.push_back(TextBlock{"第一问"});
    history.push_back(first);
    Message answer;
    answer.role = Role::Assistant;
    answer.content.push_back(TextBlock{"第一答"});
    history.push_back(answer);
    Message second;
    second.role = Role::User;
    second.content.push_back(TextBlock{"第二问"});
    history.push_back(second);
    return history;
}

}  // namespace

TEST_CASE("TranscriptUiController:空账本上焦点/查看键不消费") {
    const lubancode::cli::Theme theme;
    std::ostringstream out;
    auto controller = MakeController(out, theme);
    CHECK_FALSE(controller.HandleKey(lubancode::cli::UiKeyAction::FocusOlder));
    CHECK_FALSE(controller.HandleKey(lubancode::cli::UiKeyAction::FocusNewer));
    CHECK_FALSE(controller.HandleKey(lubancode::cli::UiKeyAction::FocusView));
    CHECK_FALSE(controller.HandleKey(lubancode::cli::UiKeyAction::ToScrollback));
    lubancode::cli::TermPort().Reset();
}

TEST_CASE("TranscriptUiController:焦点导航起手落最新,到头停住") {
    const lubancode::cli::Theme theme;
    std::ostringstream out;
    auto controller = MakeController(out, theme);
    auto& items = controller.items();
    items.push_back({});
    items.push_back({});
    items.push_back({});
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::FocusOlder));
    // 状态行给"第 3/3 条"——起手最新。
    CHECK(out.str().find("3/3") != std::string::npos);
    out.str("");
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::FocusOlder));
    CHECK(out.str().find("2/3") != std::string::npos);
    // 再两次到最老一条,之后停住不绕圈。
    (void)controller.HandleKey(lubancode::cli::UiKeyAction::FocusOlder);
    out.str("");
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::FocusOlder));
    CHECK(out.str().find("1/3") != std::string::npos);
    out.str("");
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::FocusOlder));
    CHECK(out.str().find("1/3") != std::string::npos);
    // 往新走回到最新。
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::FocusNewer));
    lubancode::cli::TermPort().Reset();
}

TEST_CASE("TranscriptUiController:Ctrl+E 进查看态,Esc 退出并重铺横幅") {
    const lubancode::cli::Theme theme;
    std::ostringstream out;
    auto controller = MakeController(out, theme);
    auto& items = controller.items();
    items.push_back({});
    bool banner_repainted = false;
    lubancode::cli::TranscriptUiController::Hooks hooks;
    hooks.repaint_banner = [&banner_repainted] { banner_repainted = true; };
    controller.SetHooks(std::move(hooks));
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::FocusView));
    // 再按 Ctrl+E = 返回:横幅重铺、最近条目摘要。
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::FocusView));
    CHECK(banner_repainted);
    // 查看态退出后 Esc 还给编辑器(不消费)。
    CHECK_FALSE(controller.HandleKey(lubancode::cli::UiKeyAction::Escape));
    lubancode::cli::TermPort().Reset();
}

TEST_CASE("TranscriptUiController:ExitFocusView 复位,下一次 Ctrl+E 是重新聚焦") {
    const lubancode::cli::Theme theme;
    std::ostringstream out;
    auto controller = MakeController(out, theme);
    controller.items().push_back({});
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::FocusView));
    controller.ExitFocusView();
    // 复位后再按 Ctrl+E 是重新进入(不是"返回"),两条路都消费键。
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::FocusView));
    lubancode::cli::TermPort().Reset();
}

TEST_CASE("TranscriptUiController:轮次导航从注入的活历史数轮,ESC 急停走钩子") {
    const lubancode::cli::Theme theme;
    std::ostringstream out;
    auto controller = MakeController(out, theme);
    const std::vector<lubancode::api::Message> history = TwoTurnHistory();
    int stopped_loops = 0;
    lubancode::cli::TranscriptUiController::Hooks hooks;
    hooks.history = [&history]() -> const std::vector<lubancode::api::Message>* { return &history; };
    hooks.stop_active_loops = [&stopped_loops]() -> int { return stopped_loops; };
    controller.SetHooks(std::move(hooks));
    // {:起手最近一轮(第 2/2 轮),再 { 回到第 1/2 轮。
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::PrevUserTurn));
    REQUIRE(out.str().find("2/2") != std::string::npos);
    out.str("");
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::PrevUserTurn));
    REQUIRE(out.str().find("1/2") != std::string::npos);
    // }:往新走回第 2/2 轮。
    out.str("");
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::NextUserTurn));
    REQUIRE(out.str().find("2/2") != std::string::npos);
    // ESC 急停:钩子报停了 1 只 → 消费并打停报。
    stopped_loops = 1;
    out.str("");
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::Escape));
    CHECK(out.str().find("已停 1 只") != std::string::npos);
    // 钩子报 0(没活 loop)→ 不消费,键还给编辑器。
    stopped_loops = 0;
    CHECK_FALSE(controller.HandleKey(lubancode::cli::UiKeyAction::Escape));
    lubancode::cli::TermPort().Reset();
}

TEST_CASE("TranscriptUiController:查看态视口构建走钩子") {
    const lubancode::cli::Theme theme;
    std::ostringstream out;
    auto controller = MakeController(out, theme);
    lubancode::cli::TranscriptUiController::Hooks hooks;
    hooks.build_task_transcript = [](int task_id, int width) {
        return std::vector<std::string>{"任务" + std::to_string(task_id) + " 第" + std::to_string(width) + "列"};
    };
    controller.SetHooks(std::move(hooks));
    controller.PrintViewedTranscript(7);
    REQUIRE(out.str().find("任务7 第80列") != std::string::npos);
    lubancode::cli::TermPort().Reset();
}

// ---------------------------------------------------------------------------
// Ctrl+O 最近一条档的重绘保档(截图单 §二.3):RepaintScreen(Ctrl+L/resize)
// 原先只传全局 expanded_,最近条目的展开选择在整屏重建后丢失。现在快照路
// 与 TurnView 重放路都要带上 expand_latest_。
// ---------------------------------------------------------------------------
TEST_CASE("TranscriptUiController:RepaintScreen 无 TurnView 时最近条目档随重画走") {
    const lubancode::cli::Theme theme;
    std::ostringstream out;
    auto controller = MakeController(out, theme);
    auto& items = controller.items();
    lubancode::cli::TranscriptItem first;
    first.id = 1;
    first.title = "run_command(git log)";
    first.summary_lines = {"Done · 退出码 0"};
    first.status = lubancode::cli::TranscriptStatus::Ok;
    items.push_back(first);
    lubancode::cli::TranscriptItem latest;
    latest.id = 2;
    latest.title = "read_file(a.txt)";
    latest.input_json = "{\"path\":\"a.txt\"}";
    latest.full_output = "1  hi";
    latest.status = lubancode::cli::TranscriptStatus::Ok;
    items.push_back(latest);

    // Ctrl+O 展开:expand_latest_ 落在最近一条(read_file)。
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::ToggleExpand));
    out.str("");
    // Ctrl+L:无 TurnView 存档,重画最近 10 条——最近一条保持展开档。
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::RepaintScreen));
    const std::string repainted = out.str();
    CHECK(repainted.find("read_file(a.txt)") != std::string::npos);
    CHECK(repainted.find("参数: {\"path\":\"a.txt\"}") != std::string::npos);  // 最近一条展开
    CHECK(repainted.find("run_command(git log)") != std::string::npos);
    bool first_expanded = repainted.find("{\"command\"") != std::string::npos;
    CHECK_FALSE(first_expanded);  // 其余条目照紧凑

    // 再按 Ctrl+O 收起,重画恢复全紧凑。
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::ToggleExpand));
    out.str("");
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::RepaintScreen));
    CHECK(out.str().find("参数:") == std::string::npos);
    lubancode::cli::TermPort().Reset();
}

TEST_CASE("TranscriptUiController:RepaintScreen 有 TurnView 时末轮最近条目保持展开") {
    const lubancode::cli::Theme theme;
    std::ostringstream out;
    auto controller = MakeController(out, theme);

    std::vector<lubancode::runtime::TurnView> views;
    // 首轮:一枚工具。
    {
        lubancode::runtime::TurnView v;
        v.turn_id = "t1";
        lubancode::runtime::TurnItemView tool;
        tool.item_id = "a1";
        tool.step_id = "s0";
        tool.kind = lubancode::runtime::TurnItemViewKind::Tool;
        tool.status = lubancode::runtime::TurnItemViewState::Succeeded;
        tool.tool_name = "run_command";
        tool.input = nlohmann::json{{"command", "git log"}};
        tool.result_text = "ok";
        v.items.push_back(tool);
        v.steps.push_back(lubancode::runtime::ModelStepView{"s0", 0, {"a1"}, true});
        views.push_back(std::move(v));
    }
    // 末轮:思考 + 工具,最近可展开条目是工具(打印序 1)。
    {
        lubancode::runtime::TurnView v;
        v.turn_id = "t2";
        lubancode::runtime::TurnItemView think;
        think.item_id = "b1";
        think.step_id = "s1";
        think.kind = lubancode::runtime::TurnItemViewKind::Thinking;
        think.status = lubancode::runtime::TurnItemViewState::Succeeded;
        think.tool_name = "thinking";
        think.result_text = "琢磨了一下";
        think.started_at_ms = 1000;
        think.ended_at_ms = 2800;
        v.items.push_back(think);
        lubancode::runtime::TurnItemView tool;
        tool.item_id = "b2";
        tool.step_id = "s1";
        tool.kind = lubancode::runtime::TurnItemViewKind::Tool;
        tool.status = lubancode::runtime::TurnItemViewState::Succeeded;
        tool.tool_name = "read_file";
        tool.input = nlohmann::json{{"path", "a.txt"}};
        tool.result_text = "1  hi";
        v.items.push_back(tool);
        v.steps.push_back(lubancode::runtime::ModelStepView{"s1", 0, {"b1", "b2"}, true});
        views.push_back(std::move(v));
    }

    lubancode::cli::TranscriptUiController::Hooks hooks;
    hooks.turn_views = [&views]() -> const std::vector<lubancode::runtime::TurnView>* { return &views; };
    controller.SetHooks(std::move(hooks));

    // Ctrl+O 展开最近一条(末轮的 read_file),Ctrl+L 重放:展开档不丢。
    controller.items().push_back(lubancode::cli::TranscriptItem{});  // ToggleExpand 需要非空账本
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::ToggleExpand));
    out.str("");
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::RepaintScreen));
    const std::string repainted = out.str();
    CHECK(repainted.find("read_file(a.txt)") != std::string::npos);
    bool read_file_expanded = repainted.find("参数: {\"path\":\"a.txt\"}") != std::string::npos;
    CHECK(read_file_expanded);  // 末轮最近条目保持展开
    bool first_turn_expanded = repainted.find("参数: {\"command\":\"git log\"}") != std::string::npos;
    CHECK_FALSE(first_turn_expanded);  // 别的轮/条目照紧凑
    CHECK(repainted.find("琢磨了一下") == std::string::npos);  // 思考照紧凑(正文不露)
    CHECK(repainted.find("(Ctrl+O 展开)") != std::string::npos);  // 思考收起档提示展开
    lubancode::cli::TermPort().Reset();
}

// ---------------------------------------------------------------------------
// 后台通知标题分家(后台代理管控三连 bug 单,Bug A):权限拒绝与监督提醒
// 各挂各的标题,不许张冠李戴。真机实录:三只后台代理工具全放行,监督器
// 的提醒 toast 却顶着"权限未放行已拒"的标题连刷五条,用户读成全线被拒。
// tr 查不到 key 回退 key 原文——断言"key 必须解析成真文案"在旧码
// (没有 supervisor 标题键)上必红。
// ---------------------------------------------------------------------------
TEST_CASE("后台通知标题分家:权限拒绝与监督提醒各挂各的标题") {
    const std::string denial = lubancode::cli::BackgroundNoticeTitle(/*permission_denial=*/true);
    const std::string supervisor = lubancode::cli::BackgroundNoticeTitle(/*permission_denial=*/false);
    // 两枚 key 都得有真文案(回退 key 原文 = 缺文案,红)。
    CHECK(denial != "agent_panel.denial_notice_title");
    CHECK(supervisor != "agent_panel.supervisor_notice_title");
    // 张冠李戴的病灶形态就是"两类同文"——必须分家。
    CHECK(denial != supervisor);
    CHECK(denial.find("权限") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 查看态 Esc 兜底(后台代理管控三连 bug 单,Bug C):Esc 流到
// TranscriptUiController 时,查看态必须一击退出回 main。旧码 Escape 分支
// 只认 Ctrl+E 聚焦查看与 loop 急停,查看态(Enter 进的 agent view)零处理
// ——Esc 落回编辑器老语义(清空),composer 本就空,屏上无响应,用户困在
// 查看页。
// ---------------------------------------------------------------------------
TEST_CASE("TranscriptUiController:查看态 Esc 一击退出回 main") {
    const lubancode::cli::Theme theme;
    std::ostringstream out;
    auto controller = MakeController(out, theme);

    // 前置:面板状态机进 #2 的查看态(Down 聚焦选中 → Enter 查看)。
    lubancode::cli::PanelSessionSlot().Reset();
    const std::vector<int> ids{2, 3};
    const auto now = std::chrono::steady_clock::now();
    const auto focus = lubancode::cli::PanelSessionSlot().HandleKey(lubancode::cli::PanelKey::Down, ids,
                                                                    /*composer_empty=*/true, now);
    REQUIRE(focus.consumed);
    const auto enter = lubancode::cli::PanelSessionSlot().HandleKey(lubancode::cli::PanelKey::EnterView, ids,
                                                                    /*composer_empty=*/true, now);
    REQUIRE(enter.consumed);
    REQUIRE(lubancode::cli::CurrentAgentViewedTaskId() == 2);

    // Esc:查看态一击退出(旧码 return false,红)。
    REQUIRE(controller.HandleKey(lubancode::cli::UiKeyAction::Escape));
    CHECK(lubancode::cli::CurrentAgentViewedTaskId() == 0);
    CHECK(out.str().find("已回主会话") != std::string::npos);

    // 退出后再按 Esc:不再是查看态,落回既有语义(loop 急停钩子没装 → 不消费,
    // 编辑器老语义不动)。
    CHECK_FALSE(controller.HandleKey(lubancode::cli::UiKeyAction::Escape));

    lubancode::cli::PanelSessionSlot().Reset();
    lubancode::cli::TermPort().Reset();
}
