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
