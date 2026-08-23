#include <doctest/doctest.h>

#include "cli/choice_menu.hpp"

using lubancode::cli::ChoiceMenuCore;
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
