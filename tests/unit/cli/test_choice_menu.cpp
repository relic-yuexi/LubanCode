#include <doctest/doctest.h>

#include "cli/choice_menu.hpp"
#include "cli/console_input.hpp"

using lubancode::cli::ChoiceMenuCore;
using lubancode::cli::ChoiceMenuItem;
using lubancode::cli::ChoiceMenuOptions;
using lubancode::cli::ChoiceMenuSearchCore;
using lubancode::cli::ChoiceMenuSearchWindowRows;
using lubancode::cli::KeyEvent;
using lubancode::cli::KeyKind;

TEST_CASE("choice menu: 单选上下循环并由 Enter 落定当前项") {
    ChoiceMenuCore menu(3, false);
    CHECK(menu.state().cursor == 0);

    menu.HandleKey(KeyEvent::Simple(KeyKind::Up));
    CHECK(menu.state().cursor == 2);
    menu.HandleKey(KeyEvent::Simple(KeyKind::Down));
    CHECK(menu.state().cursor == 0);
    menu.HandleKey(KeyEvent::Simple(KeyKind::Down));
    menu.HandleKey(KeyEvent::Simple(KeyKind::Enter));

    CHECK(menu.state().submitted);
    CHECK(menu.SelectedIndices() == std::vector<std::size_t>{1});
}

TEST_CASE("choice menu: 多选用空格勾选,空选择不许提交") {
    ChoiceMenuCore menu(3, true);
    menu.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK_FALSE(menu.state().submitted);
    CHECK(menu.state().invalid);

    menu.HandleKey(KeyEvent::Char(U' '));
    menu.HandleKey(KeyEvent::Simple(KeyKind::Down));
    menu.HandleKey(KeyEvent::Char(U' '));
    menu.HandleKey(KeyEvent::Simple(KeyKind::Enter));

    CHECK(menu.state().submitted);
    CHECK(menu.SelectedIndices() == std::vector<std::size_t>{0, 1});
}

TEST_CASE("choice menu: Esc 取消") {
    ChoiceMenuCore menu(2, false);
    menu.HandleKey(KeyEvent::Simple(KeyKind::Esc));
    CHECK(menu.state().cancelled);
    CHECK_FALSE(menu.state().submitted);
    CHECK(menu.SelectedIndices().empty());
}

TEST_CASE("choice menu: 末项可直接输入中文并提交，不先提交一个伪选项") {
    ChoiceMenuCore menu(3, false, 2);
    menu.HandleKey(KeyEvent::Char(U'灰'));  // 在任意项直接打字都会跳到末项输入框
    menu.HandleKey(KeyEvent::Char(U'度'));
    menu.HandleKey(KeyEvent::Char(U'发'));
    menu.HandleKey(KeyEvent::Char(U'布'));
    CHECK(menu.state().cursor == 2);
    CHECK(menu.state().custom_text == "灰度发布");
    CHECK(menu.SelectedIndices().empty());

    menu.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(menu.state().submitted);
    CHECK(menu.state().custom_submitted);
    CHECK(menu.SelectedIndices().empty());
}

TEST_CASE("choice menu: 自填空答案不提交，退格按 UTF-8 码点删除") {
    ChoiceMenuCore menu(3, false, 2);
    menu.HandleKey(KeyEvent::Simple(KeyKind::End));
    menu.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK_FALSE(menu.state().submitted);
    CHECK(menu.state().invalid);

    menu.HandleKey(KeyEvent::Paste("alpha中文"));
    menu.HandleKey(KeyEvent::Simple(KeyKind::Backspace));
    CHECK(menu.state().custom_text == "alpha中");
    menu.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(menu.state().custom_submitted);
}

TEST_CASE("choice menu: 多选的普通勾选与末项自填可一并提交") {
    ChoiceMenuCore menu(3, true, 2);
    menu.HandleKey(KeyEvent::Char(U' '));
    menu.HandleKey(KeyEvent::Simple(KeyKind::End));
    menu.HandleKey(KeyEvent::Paste("补充选项"));
    menu.HandleKey(KeyEvent::Simple(KeyKind::Enter));

    CHECK(menu.state().submitted);
    CHECK(menu.state().custom_submitted);
    CHECK(menu.SelectedIndices() == std::vector<std::size_t>{0});
    CHECK(menu.state().custom_text == "补充选项");
}

// ---------------------------------------------------------------------------
// 搜索 + 分页(ChoiceMenuSearchCore,长菜单用;阈值以下仍走上面的老路径,
// 老测试原样全过就是"行为不变"的证明)。
// ---------------------------------------------------------------------------

TEST_CASE("choice menu options: 搜索阈值默认 12,默认不开 always_search 且不限制窗口") {
    ChoiceMenuOptions options;
    CHECK(options.search_threshold == 12);
    CHECK_FALSE(options.always_search);
    CHECK_FALSE(options.max_visible_rows.has_value());
}

TEST_CASE("choice menu search: 向导上限钳住选项窗并给搜索栏与 hint 留位") {
    CHECK(ChoiceMenuSearchWindowRows(27, std::nullopt) == 25);
    CHECK(ChoiceMenuSearchWindowRows(27, 10) == 10);
    CHECK(ChoiceMenuSearchWindowRows(8, 10) == 6);
    CHECK(ChoiceMenuSearchWindowRows(2, 0) == 1);
}

TEST_CASE("choice menu search: 键入过滤、退格恢复、Enter 返回原索引") {
    std::vector<ChoiceMenuItem> items{{"alpha", ""}, {"beta", ""}, {"gamma", ""}, {"zeta", "zephyr"}};
    ChoiceMenuSearchCore menu(items, false);
    menu.SetWindowRows(10);

    menu.HandleKey(KeyEvent::Char(U't'));
    CHECK(menu.search() == "t");
    CHECK(menu.view() == std::vector<std::size_t>{1, 3});  // beta/zeta
    CHECK(menu.view_cursor() == 0);  // 重筛后选中跳到过滤视图第一项

    menu.HandleKey(KeyEvent::Simple(KeyKind::Backspace));
    CHECK(menu.search().empty());
    REQUIRE(menu.view().size() == 4);  // 搜索词空 = 全量
    CHECK(menu.view()[menu.view_cursor()] == 1);  // 退格守住原选中项 beta

    menu.HandleKey(KeyEvent::Char(U'z'));
    CHECK(menu.view() == std::vector<std::size_t>{3});
    menu.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(menu.state().submitted);
    CHECK(menu.SelectedIndices() == std::vector<std::size_t>{3});  // 原索引
}

TEST_CASE("choice menu search: 窗口滚动与翻页") {
    std::vector<ChoiceMenuItem> items;
    for (int i = 0; i < 10; ++i) {
        items.push_back({"item" + std::to_string(i), ""});
    }
    ChoiceMenuSearchCore menu(items, false, std::nullopt, 4);
    CHECK(menu.view_cursor() == 4);  // 初始光标按原索引定位
    menu.SetWindowRows(3);
    CHECK(menu.scroll() == 2);  // 窗口围绕选中项对齐:4 落在 [2,4]

    for (int i = 0; i < 6; ++i) {
        menu.HandleKey(KeyEvent::Simple(KeyKind::Down));
    }
    CHECK(menu.view_cursor() == 9);  // 到尾不绕圈
    CHECK(menu.scroll() == 7);       // 滚窗跟上:[7,9]

    menu.HandleKey(KeyEvent::Simple(KeyKind::PageUp));
    CHECK(menu.view_cursor() == 6);
    CHECK(menu.scroll() == 6);

    menu.HandleKey(KeyEvent::Simple(KeyKind::PageDown));
    CHECK(menu.view_cursor() == 9);
    CHECK(menu.scroll() == 7);

    menu.HandleKey(KeyEvent::Simple(KeyKind::Home));
    CHECK(menu.view_cursor() == 0);
    CHECK(menu.scroll() == 0);

    menu.HandleKey(KeyEvent::Simple(KeyKind::Up));  // 到头不绕圈
    CHECK(menu.view_cursor() == 0);

    menu.HandleKey(KeyEvent::Simple(KeyKind::End));
    CHECK(menu.view_cursor() == 9);
    CHECK(menu.scroll() == 7);
}

TEST_CASE("choice menu search: 多选勾选写回原索引") {
    std::vector<ChoiceMenuItem> items{
        {"ant", ""}, {"bee", ""}, {"cat", ""}, {"dog", ""}, {"elk", ""}, {"fox", ""}};
    ChoiceMenuSearchCore menu(items, true);
    menu.SetWindowRows(4);

    menu.HandleKey(KeyEvent::Char(U'o'));  // dog/fox
    REQUIRE(menu.view() == std::vector<std::size_t>{3, 5});
    menu.HandleKey(KeyEvent::Char(U' '));             // 勾 dog(原索引 3)
    menu.HandleKey(KeyEvent::Simple(KeyKind::Down));  // 到 fox
    menu.HandleKey(KeyEvent::Char(U' '));             // 勾 fox(原索引 5)

    menu.HandleKey(KeyEvent::Simple(KeyKind::Backspace));  // 清搜索词,守住 fox
    REQUIRE(menu.search().empty());
    CHECK(menu.view()[menu.view_cursor()] == 5);
    menu.HandleKey(KeyEvent::Simple(KeyKind::Up));  // elk(原索引 4)
    menu.HandleKey(KeyEvent::Char(U' '));           // 勾 elk

    menu.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(menu.state().submitted);
    CHECK(menu.SelectedIndices() == std::vector<std::size_t>{3, 4, 5});
}

TEST_CASE("choice menu search: editable 项恒显,光标在其上时键入走行内文本") {
    std::vector<ChoiceMenuItem> items{{"ant", ""}, {"bee", ""}, {"custom", ""}};
    ChoiceMenuSearchCore menu(items, false, 2);
    menu.SetWindowRows(5);

    menu.HandleKey(KeyEvent::Char(U'x'));  // 无命中,视图只剩恒显的 editable
    REQUIRE(menu.view() == std::vector<std::size_t>{2});
    CHECK(menu.cursor_on_editable());
    CHECK(menu.search() == "x");

    menu.HandleKey(KeyEvent::Char(U'自'));  // 光标在 editable 上:进行内文本
    CHECK(menu.state().custom_text == "自");
    CHECK(menu.search() == "x");

    menu.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK(menu.state().custom_submitted);
    CHECK(menu.state().submitted);
}

TEST_CASE("choice menu search: 光标不在 editable 上时键入进搜索词") {
    std::vector<ChoiceMenuItem> items{{"ant", ""}, {"bee", ""}, {"custom", ""}};
    ChoiceMenuSearchCore menu(items, false, 2);
    menu.SetWindowRows(5);

    CHECK_FALSE(menu.cursor_on_editable());
    menu.HandleKey(KeyEvent::Char(U'a'));  // 不在 editable 上 → 搜索词
    CHECK(menu.search() == "a");
    CHECK(menu.view() == std::vector<std::size_t>{0, 2});  // ant + 恒显 editable
    CHECK(menu.state().custom_text.empty());
}

TEST_CASE("choice menu search: 大小写不敏感,description 也参与匹配") {
    std::vector<ChoiceMenuItem> items{{"OpenAI", "gpt-4o"}, {"Anthropic", "claude 3.5"}};
    ChoiceMenuSearchCore menu(items, false);
    menu.SetWindowRows(5);

    menu.HandleKey(KeyEvent::Char(U'C'));
    menu.HandleKey(KeyEvent::Char(U'L'));
    CHECK(menu.search() == "CL");
    CHECK(menu.view() == std::vector<std::size_t>{1});  // 命中 description "claude"
}

TEST_CASE("choice menu search: OpenRouter 全词留下两种 wire") {
    std::vector<ChoiceMenuItem> items{{"OpenRouter", "OpenAI-compatible Chat API"},
                                      {"OpenRouter (Anthropic)", "Anthropic Messages API"},
                                      {"Perplexity", "OpenAI-compatible Chat API"}};
    ChoiceMenuSearchCore menu(items, false);
    for (const char ch : std::string("openrouter")) {
        menu.HandleKey(KeyEvent::Char(static_cast<char32_t>(ch)));
    }

    CHECK(menu.search() == "openrouter");
    CHECK(menu.view() == std::vector<std::size_t>{0, 1});
    CHECK(menu.view_cursor() == 0);
}

TEST_CASE("choice menu search: 搜空 Enter 不提交,Esc 取消") {
    std::vector<ChoiceMenuItem> items{{"a", ""}, {"b", ""}};
    ChoiceMenuSearchCore menu(items, false);
    menu.SetWindowRows(5);

    menu.HandleKey(KeyEvent::Char(U'z'));
    CHECK(menu.view().empty());
    menu.HandleKey(KeyEvent::Simple(KeyKind::Enter));
    CHECK_FALSE(menu.state().submitted);
    CHECK(menu.state().invalid);

    menu.HandleKey(KeyEvent::Simple(KeyKind::Esc));
    CHECK(menu.state().cancelled);
    CHECK(menu.SelectedIndices().empty());
}
