// /provider 选择面板(Provider选择面板单)的纯逻辑钉子:条目生成(名字/
// 当前端标记/鉴权缺失短标记)、按键状态机(上下移动 + 滚动窗口 + 翻页)、
// 帧拼装。TTY 宿主(RunProviderPanel)要真终端,归真机手测;裸敲
// /provider 与裸敲 /provider switch 的接线(HandleProviderCommand)要整
// 套后端栈,归 app 层,不在册内。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/i18n.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8(窄终端截行的宽度口径)
#include "cli/provider_panel_core.hpp"
#include "config/config.hpp"

using namespace lubancode;

namespace {

constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

cli::KeyEvent Key(cli::KeyKind kind) {
    return cli::KeyEvent::Simple(kind);
}

config::ProviderConfig MakeProvider(const std::string& name, config::ProviderAuthMode auth,
                                    const std::string& key_env = "", const std::string& api_key = "") {
    config::ProviderConfig provider;
    provider.name = name;
    provider.base_url = "https://" + name + ".test/v1";
    provider.auth = auth;
    provider.key_env = key_env;
    provider.api_key = api_key;
    provider.model = "m";
    return provider;
}

std::vector<cli::ProviderPanelEntry> MakeEntries() {
    return {
        {"alpha", true, ""},
        {"beta", false, ""},
        {"gamma", false, cli::tr("provider_panel.auth_missing")},
    };
}

}  // namespace

// ---------------------------------------------------------------------------
// 条目生成
// ---------------------------------------------------------------------------

TEST_CASE("BuildProviderPanelEntries: 名字/当前端标记/鉴权缺失短标记") {
    std::vector<config::ProviderConfig> providers = {
        MakeProvider("local", config::ProviderAuthMode::None),
        // 测试环境不会设这枚变量:按缺密钥计(与 test_provider_switch 同款
        // 手法,变量名起得更偏,防 CI 机器恰好设过)。
        MakeProvider("minimax", config::ProviderAuthMode::Env, "LUBANCODE_TEST_UNSET_KEY_XYZ"),
        MakeProvider("custom", config::ProviderAuthMode::Inline, "", "sk-plain-123"),
        MakeProvider("half", config::ProviderAuthMode::Inline),
    };
    const auto entries = cli::BuildProviderPanelEntries(providers, "minimax");
    REQUIRE(entries.size() == 4);
    // 顺序照配置排列,名字原样。
    CHECK(entries[0].name == "local");
    CHECK(entries[1].name == "minimax");
    CHECK(entries[2].name == "custom");
    CHECK(entries[3].name == "half");
    // 当前端只打一枚标记。
    CHECK_FALSE(entries[0].is_current);
    CHECK(entries[1].is_current);
    CHECK_FALSE(entries[2].is_current);
    // 鉴权:缺的才给短标记,齐备/无需鉴权不打标(面板不是鉴权体检表)。
    CHECK(entries[0].auth_note.empty());       // none
    CHECK(entries[1].auth_note == cli::tr("provider_panel.auth_missing"));  // env 缺变量
    CHECK(entries[2].auth_note.empty());       // inline 有 key
    CHECK(entries[3].auth_note == cli::tr("provider_panel.auth_missing"));  // inline 缺 key
    // 任何标记都不带明文 key。
    for (const auto& entry : entries) {
        CHECK(entry.auth_note.find("sk-plain-123") == std::string::npos);
    }
}

TEST_CASE("BuildProviderPanelEntries: 当前端不在清单里则无人打标") {
    const auto entries = cli::BuildProviderPanelEntries(
        {MakeProvider("a", config::ProviderAuthMode::None)}, "别的家");
    REQUIRE(entries.size() == 1);
    CHECK_FALSE(entries[0].is_current);
}

TEST_CASE("FindProviderPanelIndex: 精确等值找下标,空名/未命中给 npos") {
    const auto entries = MakeEntries();
    CHECK(cli::FindProviderPanelIndex(entries, "alpha") == 0);
    CHECK(cli::FindProviderPanelIndex(entries, "gamma") == 2);
    CHECK(cli::FindProviderPanelIndex(entries, "Beta") == kNpos);  // 大小写敏感:按精确名
    CHECK(cli::FindProviderPanelIndex(entries, "") == kNpos);
    CHECK(cli::FindProviderPanelIndex(entries, "nope") == kNpos);
    CHECK(cli::FindProviderPanelIndex({}, "alpha") == kNpos);
}

// ---------------------------------------------------------------------------
// 按键状态机:上下移动/环绕/单项与空清单边界
// ---------------------------------------------------------------------------

TEST_CASE("ProviderPanelCore: 上下移动并首尾环绕,Tab/ShiftTab 是同义键") {
    cli::ProviderPanelCore core(3, 3, 0);
    CHECK(core.state().cursor == 0);
    core.HandleKey(Key(cli::KeyKind::Down));
    CHECK(core.state().cursor == 1);
    core.HandleKey(Key(cli::KeyKind::Down));
    CHECK(core.state().cursor == 2);
    core.HandleKey(Key(cli::KeyKind::Down));
    CHECK(core.state().cursor == 0);  // 下到尾绕回首项
    core.HandleKey(Key(cli::KeyKind::Up));
    CHECK(core.state().cursor == 2);  // 上到头绕回尾项
    core.HandleKey(Key(cli::KeyKind::Tab));
    CHECK(core.state().cursor == 0);
    core.HandleKey(Key(cli::KeyKind::ShiftTab));
    CHECK(core.state().cursor == 2);
}

TEST_CASE("ProviderPanelCore: 单项清单边界——环绕等于原地") {
    cli::ProviderPanelCore core(1, 1, 0);
    core.HandleKey(Key(cli::KeyKind::Down));
    core.HandleKey(Key(cli::KeyKind::Up));
    core.HandleKey(Key(cli::KeyKind::End));
    core.HandleKey(Key(cli::KeyKind::Home));
    CHECK(core.state().cursor == 0);
    CHECK(core.state().offset == 0);
    CHECK_FALSE(core.state().submitted);
}

TEST_CASE("ProviderPanelCore: 空清单——键不挪光标,Enter 不算选中") {
    cli::ProviderPanelCore core(0, 5, 3);  // 越界起点钳回首项
    CHECK(core.state().cursor == 0);
    core.HandleKey(Key(cli::KeyKind::Down));
    core.HandleKey(Key(cli::KeyKind::Up));
    core.HandleKey(Key(cli::KeyKind::End));
    core.HandleKey(Key(cli::KeyKind::PageDown));
    CHECK(core.state().cursor == 0);
    core.HandleKey(Key(cli::KeyKind::Enter));
    CHECK_FALSE(core.state().submitted);  // 没有可选项,Enter 落空
    core.HandleKey(Key(cli::KeyKind::Esc));
    CHECK(core.state().cancelled);
}

TEST_CASE("ProviderPanelCore: 进场光标钳进清单") {
    cli::ProviderPanelCore core(4, 3, 9);  // 越界起点
    CHECK(core.state().cursor == 0);
    cli::ProviderPanelCore core2(4, 3, 3);
    CHECK(core2.state().cursor == 3);
    CHECK(core2.state().offset == 0);  // 窗口尚未出界,不预滚
}

// ---------------------------------------------------------------------------
// 按键状态机:滚动窗口
// ---------------------------------------------------------------------------

TEST_CASE("ProviderPanelCore: 光标出窗,窗口跟着挪;回绕也带窗口走") {
    cli::ProviderPanelCore core(8, 3, 0);
    core.HandleKey(Key(cli::KeyKind::Down));  // cursor 1
    core.HandleKey(Key(cli::KeyKind::Down));  // cursor 2
    CHECK(core.state().offset == 0);          // 尚在窗口内
    core.HandleKey(Key(cli::KeyKind::Down));  // cursor 3,出窗
    CHECK(core.state().cursor == 3);
    CHECK(core.state().offset == 1);
    core.HandleKey(Key(cli::KeyKind::End));   // cursor 7
    CHECK(core.state().offset == 5);          // 窗口贴尾:5..7
    core.HandleKey(Key(cli::KeyKind::Home));  // cursor 0
    CHECK(core.state().offset == 0);
    core.HandleKey(Key(cli::KeyKind::Up));    // 绕回尾项 7
    CHECK(core.state().cursor == 7);
    CHECK(core.state().offset == 5);          // 环绕后窗口也贴尾
    core.HandleKey(Key(cli::KeyKind::Up));    // cursor 6,仍在窗口
    CHECK(core.state().offset == 5);
}

TEST_CASE("ProviderPanelCore: PageUp/PageDown 整屏翻,页界钳住不绕") {
    cli::ProviderPanelCore core(9, 3, 0);
    core.HandleKey(Key(cli::KeyKind::PageDown));  // 0 -> 3
    CHECK(core.state().cursor == 3);
    CHECK(core.state().offset == 3);
    core.HandleKey(Key(cli::KeyKind::PageDown));  // 3 -> 6
    core.HandleKey(Key(cli::KeyKind::PageDown));  // 6 -> 8(尾项,钳住)
    CHECK(core.state().cursor == 8);
    CHECK(core.state().offset == 6);
    core.HandleKey(Key(cli::KeyKind::PageUp));    // 8 -> 5
    CHECK(core.state().cursor == 5);
    core.HandleKey(Key(cli::KeyKind::PageUp));    // 5 -> 2
    core.HandleKey(Key(cli::KeyKind::PageUp));    // 2 -> 0(首项,钳住)
    CHECK(core.state().cursor == 0);
    CHECK(core.state().offset == 0);
}

// ---------------------------------------------------------------------------
// 按键状态机:确认/取消/死键
// ---------------------------------------------------------------------------

TEST_CASE("ProviderPanelCore: Enter 确认,Esc/Ctrl+C/Ctrl+D 取消,收场后键落空") {
    SUBCASE("Enter") {
        cli::ProviderPanelCore core(3, 2, 1);
        core.HandleKey(Key(cli::KeyKind::Enter));
        CHECK(core.state().submitted);
        CHECK_FALSE(core.state().cancelled);
        core.HandleKey(Key(cli::KeyKind::Down));  // 收场后键一律落空
        CHECK(core.state().cursor == 1);
        core.HandleKey(Key(cli::KeyKind::Esc));   // 已提交,取消也落空
        CHECK_FALSE(core.state().cancelled);
    }
    SUBCASE("Esc / CtrlC / CtrlD 都当取消(Ctrl+C 不退程序)") {
        for (const cli::KeyKind kind : {cli::KeyKind::Esc, cli::KeyKind::CtrlC, cli::KeyKind::CtrlD}) {
            cli::ProviderPanelCore core(3, 2, 0);
            core.HandleKey(Key(kind));
            CHECK(core.state().cancelled);
            CHECK_FALSE(core.state().submitted);
        }
    }
}

TEST_CASE("ProviderPanelCore: 字符/退格/左右键一概不收(这块面板只挑不筛)") {
    cli::ProviderPanelCore core(3, 3, 0);
    core.HandleKey(cli::KeyEvent::Char(U'x'));
    core.HandleKey(cli::KeyEvent::Char(U'筛'));
    core.HandleKey(Key(cli::KeyKind::Backspace));
    core.HandleKey(Key(cli::KeyKind::Left));
    core.HandleKey(Key(cli::KeyKind::Right));
    core.HandleKey(Key(cli::KeyKind::CtrlO));
    CHECK(core.state().cursor == 0);
    CHECK_FALSE(core.state().submitted);
    CHECK_FALSE(core.state().cancelled);
}

// ---------------------------------------------------------------------------
// 帧拼装(语言默认 zh-CN,英文词条成对在末案)
// ---------------------------------------------------------------------------

TEST_CASE("BuildProviderPanelFrame: 布局逐行对账") {
    cli::ProviderPanelView view;
    view.entries = MakeEntries();
    view.cursor = 1;
    view.offset = 0;
    view.visible_capacity = 0;  // 0 = 不限,整表画
    const auto frame = cli::BuildProviderPanelFrame(view, 80);
    // 8 行:标题/当前端/空行/三行条目/空行/footer。
    REQUIRE(frame.lines.size() == 8);
    CHECK(frame.lines[0] == cli::tr("provider_panel.title"));
    CHECK(frame.lines[1] == cli::trf("provider_panel.current_line", std::string("alpha")));
    CHECK(frame.lines[2].empty());
    // 当前端行内标记 + 高亮行 "> " 前缀(文本标记,不只靠颜色)。
    CHECK(frame.lines[3] == "  alpha" + cli::tr("cmd.provider.current"));
    CHECK(frame.lines[4] == "> beta");
    CHECK(frame.lines[5] == "  gamma  " + cli::tr("provider_panel.auth_missing"));
    CHECK(frame.lines[6].empty());
    CHECK(frame.lines[7] == cli::tr("provider_panel.footer"));
}

TEST_CASE("BuildProviderPanelFrame: 长列表滚动窗口,上下省略行带余数") {
    std::vector<cli::ProviderPanelEntry> entries;
    for (int i = 0; i < 10; ++i) {
        entries.push_back({"p" + std::to_string(i), i == 0, ""});
    }
    cli::ProviderPanelView view;
    view.entries = entries;
    view.visible_capacity = 3;
    view.offset = 4;
    view.cursor = 5;
    const auto frame = cli::BuildProviderPanelFrame(view, 80);
    // 标题/当前端/空行/上方省略/三行条目/下方省略/空行/footer = 10 行。
    REQUIRE(frame.lines.size() == 10);
    CHECK(frame.lines[3] == cli::trf("provider_panel.more_above", std::size_t{4}));
    CHECK(frame.lines[4].rfind("  p4", 0) == 0);
    CHECK(frame.lines[5].rfind("> p5", 0) == 0);
    CHECK(frame.lines[6].rfind("  p6", 0) == 0);
    CHECK(frame.lines[7] == cli::trf("provider_panel.more_below", std::size_t{3}));
}

TEST_CASE("BuildProviderPanelFrame: 窗口恰装到表尾,不画下方省略行") {
    std::vector<cli::ProviderPanelEntry> entries;
    for (int i = 0; i < 5; ++i) {
        entries.push_back({"q" + std::to_string(i), false, ""});
    }
    cli::ProviderPanelView view;
    view.entries = entries;
    view.visible_capacity = 3;
    view.offset = 2;
    view.cursor = 4;
    const auto frame = cli::BuildProviderPanelFrame(view, 80);
    // 标题/当前端/空行/上方省略/三行条目(offset 2 + 3 行 = 全表,无下方
    // 省略)/空行/footer = 9 行。
    REQUIRE(frame.lines.size() == 9);
    CHECK(frame.lines[3] == cli::trf("provider_panel.more_above", std::size_t{2}));
    CHECK(frame.lines[4] == "  q2");
    CHECK(frame.lines[5] == "  q3");
    CHECK(frame.lines[6] == "> q4");   // cursor=4:末行是高亮行
    CHECK(frame.lines[8] == cli::tr("provider_panel.footer"));
}

TEST_CASE("BuildProviderPanelFrame: 空清单给提示行,无人打当前端") {
    cli::ProviderPanelView view;  // entries 空
    const auto frame = cli::BuildProviderPanelFrame(view, 80);
    // 标题/当前端(未设)/空行/提示/空行/footer = 6 行。
    REQUIRE(frame.lines.size() == 6);
    CHECK(frame.lines[1] == cli::tr("provider_panel.current_none"));
    CHECK(frame.lines[3] == cli::tr("cmd.provider.empty"));
}

TEST_CASE("BuildProviderPanelFrame: 窄终端按显示宽度截行,不溢出") {
    cli::ProviderPanelView view;
    view.entries = MakeEntries();
    view.cursor = 1;
    const auto frame = cli::BuildProviderPanelFrame(view, 20);
    for (const std::string& line : frame.lines) {
        CHECK(cli::DisplayWidthUtf8(line) <= 18);  // usable = 20 - 2
    }
}

TEST_CASE("BuildProviderPanelFrame: 英文词条成对(切 en 后布局仍齐)") {
    const std::string saved = cli::CurrentLanguage();
    cli::SetLanguage("en");
    cli::ProviderPanelView view;
    view.entries = MakeEntries();
    view.cursor = 0;
    const auto frame = cli::BuildProviderPanelFrame(view, 80);
    CHECK(frame.lines[0] == "Switch provider");
    CHECK(frame.lines[1] == "Current: alpha");
    CHECK(frame.lines[3].find("alpha") != std::string::npos);
    CHECK(frame.lines[3].find("(current)") != std::string::npos);
    CHECK(frame.lines[5].find("key missing") != std::string::npos);
    CHECK(frame.lines[7] == cli::tr("provider_panel.footer"));
    cli::SetLanguage(saved);
}
