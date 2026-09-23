// TUI 排版批 4(/goal 全族)的输出形状册。
//   - status/view:首行(跨壳投影头)进 frame 标题,其余按句内冒号拆键值
//     对,key 列全表对齐;
//   - create/pause/resume/edit 反馈与 already_active 错误全进键值对框(错误
//     走 error 语义色);
//   - plain 主题(默认 Theme 即 plain)钉零转义、无框、信息一字不少。
//
// 走真 HandleGoalCommand(v3 卷,夹具照 test_goal_v3_commands.cpp 的真账本
// 路:临时根 + TrajectorySessionLedger + GoalSessionWiring;TermPort 改道
// 捕获)。

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "app/commands/goal_commands.hpp"
#include "app/wirings/goal_session_wiring.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8:对齐断言按显示列量
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "runtime/trajectory_session.hpp"
#include "workspace/identity.hpp"

namespace goalns = lubancode::runtime::goal;
using lubancode::app::GoalSessionWiring;
using lubancode::app::GoalWiring;
using lubancode::cli::GoalCommandAction;
using lubancode::cli::ParsedGoalCommand;

namespace {

// ctest 钉 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0;v3 册显式开回 1(与
// test_goal_v3_commands.cpp 同一把尺)。
struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

ParsedGoalCommand ParseAction(GoalCommandAction action, std::string objective = "") {
    ParsedGoalCommand parsed;
    parsed.action = action;
    parsed.objective = std::move(objective);
    return parsed;
}

// 真账本夹具(照 test_goal_v3_commands.cpp;只留命令面要的件)。theme 成员
// 可在跑命令前换成 dark/plain,两路形状各钉一本。
struct GoalFrameFixture {
    std::filesystem::path dir;
    std::optional<lubancode::runtime::TrajectorySessionLedger> ledger;
    lubancode::cli::Theme theme;
    lubancode::config::Config config;
    GoalSessionWiring wiring;
    std::vector<std::string> turn_texts;
    std::vector<std::string> notes;
    std::ostringstream term_captured;

    explicit GoalFrameFixture() {
        std::error_code ec;
        dir = std::filesystem::temp_directory_path() /
              ("lubancode-goal-frame-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(dir, ec);
        std::filesystem::create_directories(dir / "repo", ec);
        config.features_goals = true;
        lubancode::cli::TermPort().Redirect(&term_captured, nullptr);
        lubancode::runtime::TrajectorySessionLedger::Options options;
        options.workspaces_root = dir / "workspaces";
        options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(dir / "repo");
        options.lubancode_version = "test";
        auto opened = lubancode::runtime::TrajectorySessionLedger::Open(options);
        REQUIRE(opened.has_value());
        ledger.emplace(std::move(*opened));
        GoalSessionWiring::Host host;
        host.theme = &theme;
        host.config = &config;
        host.current_model = std::make_shared<std::string>("test-model");
        host.trajectory = &*ledger;
        host.start_turn = [this](const std::string& text, bool* failed, bool* cancelled) {
            turn_texts.push_back(text);
            if (failed != nullptr) *failed = false;
            if (cancelled != nullptr) *cancelled = false;
        };
        host.notify = [this](bool is_error, const std::string& text) {
            notes.push_back(std::string(is_error ? "E: " : "N: ") + text);
        };
        wiring.AttachHost(std::move(host));
    }

    ~GoalFrameFixture() {
        lubancode::cli::TermPort().Reset();
        ledger.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    GoalFrameFixture(const GoalFrameFixture&) = delete;
    GoalFrameFixture& operator=(const GoalFrameFixture&) = delete;

    GoalWiring Pack() { return wiring.MakeCommandWiring(nullptr, nullptr); }
    std::string TakeOutput() {
        const std::string out = term_captured.str();
        term_captured.str(std::string());
        return out;
    }
};

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// 剥掉 CSI 序列(与 test_plugin_commands_frame.cpp 同一把手写的尺)。
std::string StripAnsiLight(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() &&
                   !((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z'))) {
                ++i;
            }
            if (i < text.size()) {
                ++i;  // 吃掉终结字母
            }
            continue;
        }
        out += text[i];
        ++i;
    }
    return out;
}

// key 段之后 value 的起始显示列(key 补齐 + 两格列距后应处处同列)。
int ValueStartCol(const std::string& row, const std::string& key) {
    const std::size_t at = row.find(key);
    if (at == std::string::npos) {
        return -1;
    }
    std::size_t i = at + key.size();
    while (i < row.size() && row[i] == ' ') {
        ++i;
    }
    return static_cast<int>(lubancode::cli::DisplayWidthUtf8(row.substr(0, i)));
}

constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxLightVert = "\xe2\x94\x82";     // │

}  // namespace

TEST_CASE("status: 投影头进 frame 标题,键值列全表对齐(dark)") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    GoalFrameFixture fixture;
    fixture.theme = lubancode::cli::BuiltinTheme("dark");
    fixture.wiring.Ensure(fixture.config);
    GoalWiring pack = fixture.Pack();

    lubancode::app::HandleGoalCommand(
        ParseAction(GoalCommandAction::Create, "修好 auth;ctest -R auth 全过"), pack);
    fixture.TakeOutput();

    lubancode::app::HandleGoalCommand(ParseAction(GoalCommandAction::Status), pack);
    const std::string out = fixture.TakeOutput();

    REQUIRE(Contains(out, kBoxLightTopLeft));
    // 标题(投影头)嵌上边框;key 列走 row_label 语义色。
    const std::string plain = StripAnsiLight(out);
    CHECK(Contains(plain, "goal-1"));
    CHECK(Contains(out, fixture.theme.row_label));
    CHECK(Contains(plain, "目标"));
    CHECK(Contains(plain, "预算"));
    CHECK(Contains(plain, "防空转"));
    // key 列对齐:目标/预算/防空转的 value 起始显示列一致。
    int goal_col = -1;
    int budget_col = -1;
    int streak_col = -1;
    std::istringstream lines(plain);
    std::string line;
    while (std::getline(lines, line)) {
        if (goal_col < 0) goal_col = ValueStartCol(line, "目标");
        if (budget_col < 0) budget_col = ValueStartCol(line, "预算");
        if (streak_col < 0) streak_col = ValueStartCol(line, "防空转");
    }
    CHECK(goal_col > 0);
    CHECK(budget_col > 0);
    CHECK(streak_col > 0);
    CHECK(goal_col == budget_col);
    CHECK(goal_col == streak_col);
}

TEST_CASE("create/pause/resume 反馈与 already_active 错误进 frame(dark)") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    GoalFrameFixture fixture;
    fixture.theme = lubancode::cli::BuiltinTheme("dark");
    fixture.wiring.Ensure(fixture.config);
    GoalWiring pack = fixture.Pack();

    // create 回执:键值对框 + 既有文案一字不少。
    lubancode::app::HandleGoalCommand(
        ParseAction(GoalCommandAction::Create, "修好 auth;ctest 全过"), pack);
    {
        const std::string out = fixture.TakeOutput();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        const std::string plain = StripAnsiLight(out);
        CHECK(Contains(plain, "目标已立"));
        CHECK(Contains(plain, "首轮工作项 wi-1"));
    }

    // 已有活动目标再 create:错误进框,走 error 语义色。
    lubancode::app::HandleGoalCommand(ParseAction(GoalCommandAction::Create, "第二只"), pack);
    {
        const std::string out = fixture.TakeOutput();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(out, fixture.theme.error));
        CHECK(Contains(StripAnsiLight(out), "已有一只活动目标"));
    }

    // pause → resume:两条反馈各进一只键值对框。转换表边照 test_goal_v3_
    // commands.cpp——create 落 preparing,先开一轮到位再 pause。
    const goalns::GoalStateSnapshot* current = pack.goal_service->current();
    REQUIRE(current != nullptr);
    auto claim = pack.goal_service->ClaimPendingIntent("run-frame", current->state_revision,
                                                       nlohmann::json{});
    REQUIRE(claim.ok);
    REQUIRE(pack.goal_service->BeginIteration(pack.goal_service->current()->state_revision, {})
                 .ok);
    REQUIRE(pack.goal_service->EndIteration(pack.goal_service->current()->state_revision, {}).ok);

    lubancode::app::HandleGoalCommand(ParseAction(GoalCommandAction::Pause), pack);
    {
        const std::string out = fixture.TakeOutput();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsiLight(out), "目标已暂停"));
    }
    lubancode::app::HandleGoalCommand(ParseAction(GoalCommandAction::Resume), pack);
    {
        const std::string out = fixture.TakeOutput();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsiLight(out), "目标已续"));
    }
}

TEST_CASE("plain 主题:零转义、无框,信息一字不少") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    GoalFrameFixture fixture;  // 默认 Theme = plain
    fixture.wiring.Ensure(fixture.config);
    GoalWiring pack = fixture.Pack();

    lubancode::app::HandleGoalCommand(
        ParseAction(GoalCommandAction::Create, "修好 auth;ctest 全过"), pack);
    lubancode::app::HandleGoalCommand(ParseAction(GoalCommandAction::Status), pack);
    const std::string out = fixture.TakeOutput();
    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(!Contains(out, kBoxLightTopLeft));
    CHECK(!Contains(out, kBoxLightVert));
    CHECK(Contains(out, "目标已立"));
    CHECK(Contains(out, "goal-1"));
    CHECK(Contains(out, "预算"));
}
