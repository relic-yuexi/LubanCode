// menu_text_edit(HC-04 合并单)的测试:三个菜单面板原先各养一份逐分支相同
// 的 AppendUtf8/EraseLastUtf8,合并进 cli/menu_text_edit.hpp。这里钉两样:
//   - 内核刀口:空串、ASCII、双/三/四字节、越界码点、代理项、孤立续字节、
//     组合附标与 ZWJ——全部按合并前既有行为钉死,行为零变更。
//   - 跨面板一致:同一键序列喂三块面板(ChoiceMenu 自填项/ProviderSwitch
//     筛选词/SessionPicker 搜索词),文本槽增删结果逐串相同。
//
// 删的是码点不是字素簇:组合附标、ZWJ 序列各算独立码点逐个退,整簇删除
// 另立需求,这册里钉的就是"不许悄悄变簇删"。
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/choice_menu.hpp"
#include "cli/menu_text_edit.hpp"
#include "cli/provider_switch.hpp"
#include "cli/session_picker.hpp"

using lubancode::cli::AppendUtf8;
using lubancode::cli::ChoiceMenuCore;
using lubancode::cli::EraseLastUtf8;
using lubancode::cli::KeyEvent;
using lubancode::cli::KeyKind;
using lubancode::cli::ProviderSwitchCore;
using lubancode::cli::SessionPickerCore;

namespace {

// 三档宽度 + ASCII 的黄金字节:a(1) é(2) 灰(3) 🙂(4)。
std::string BuildGoldenWidths() {
    std::string out;
    AppendUtf8(out, U'a');
    AppendUtf8(out, 0x00E9);
    AppendUtf8(out, 0x7070);
    AppendUtf8(out, 0x1F642);
    return out;
}

}  // namespace

TEST_CASE("menu_text_edit: 追加按码点宽度落盘,黄金字节逐位对齐") {
    const std::string golden = BuildGoldenWidths();
    CHECK(golden.size() == 1 + 2 + 3 + 4);
    CHECK(golden == std::string("a\xC3\xA9\xE7\x81\xB0\xF0\x9F\x99\x82"));
}

TEST_CASE("menu_text_edit: 越界码点整码点丢弃,一字节不出") {
    std::string out;
    AppendUtf8(out, U'x');
    AppendUtf8(out, 0x110000);  // 超 0x10FFFF:静默丢弃(既有行为)
    AppendUtf8(out, 0xFFFFFFFF);
    CHECK(out == "x");
}

TEST_CASE("menu_text_edit: 代理项走 3 字节支路照编(WTF-8 既有口径)") {
    std::string out;
    AppendUtf8(out, 0xD800);
    AppendUtf8(out, 0xDFFF);
    CHECK(out.size() == 6);
    CHECK(static_cast<unsigned char>(out[0]) == 0xED);  // 3 字节序列头
    CHECK(static_cast<unsigned char>(out[3]) == 0xED);
}

TEST_CASE("menu_text_edit: 尾删逐码点回退,空串安全") {
    std::string text = BuildGoldenWidths();
    EraseLastUtf8(text);
    CHECK(text == "a\xC3\xA9\xE7\x81\xB0");
    EraseLastUtf8(text);
    CHECK(text == "a\xC3\xA9");
    EraseLastUtf8(text);
    CHECK(text == "a");
    EraseLastUtf8(text);
    CHECK(text.empty());
    EraseLastUtf8(text);  // 空串再删,原样不动
    CHECK(text.empty());
}

TEST_CASE("menu_text_edit: 组合附标与 ZWJ 按码点逐个退,不整簇删") {
    SUBCASE("组合附标:删一次只掉附标,基底还在") {
        std::string text;
        AppendUtf8(text, U'e');
        AppendUtf8(text, 0x0301);  // 组合尖音符
        EraseLastUtf8(text);
        CHECK(text == "e");
    }
    SUBCASE("ZWJ 序列:删一次只掉末尾一个码点") {
        std::string text;
        AppendUtf8(text, 0x1F469);  // 👩
        AppendUtf8(text, 0x200D);   // ZWJ
        AppendUtf8(text, 0x1F4BC);  // 💻
        EraseLastUtf8(text);
        std::string expected;
        AppendUtf8(expected, 0x1F469);
        AppendUtf8(expected, 0x200D);
        CHECK(text == expected);
    }
}

TEST_CASE("menu_text_edit: 孤立续字节一路退到串首(既有口径,钉死)") {
    SUBCASE("全续字节串:整段抹空") {
        std::string text("\x80\x80");
        EraseLastUtf8(text);
        CHECK(text.empty());
    }
    SUBCASE("合法字节后挂孤立续字节:连前一字节一并退掉") {
        std::string text("a\x80");
        EraseLastUtf8(text);
        CHECK(text.empty());
    }
}

TEST_CASE("menu_text_edit: 同一键序列在三块面板文本槽上增删结果相同") {
    const std::vector<char32_t> typed = {U'g', U'r', U'a', U'y', 0x1F642, U'灰'};
    ChoiceMenuCore choice(3, false, 2);
    ProviderSwitchCore provider(5, 0);
    SessionPickerCore session(10);

    std::string expected;
    for (const char32_t ch : typed) {
        const KeyEvent ev = KeyEvent::Char(ch);
        choice.HandleKey(ev);
        provider.HandleKey(ev);
        session.HandleKey(ev);
        AppendUtf8(expected, ch);
    }
    CHECK(choice.state().custom_text == expected);
    CHECK(provider.state().filter == expected);
    CHECK(session.state().search == expected);

    // 退 4 个码点:gray🙂灰 只剩 gr,三槽步调一致。
    for (int i = 0; i < 4; ++i) {
        const KeyEvent ev = KeyEvent::Simple(KeyKind::Backspace);
        choice.HandleKey(ev);
        provider.HandleKey(ev);
        session.HandleKey(ev);
        EraseLastUtf8(expected);
    }
    CHECK(expected == "gr");
    CHECK(choice.state().custom_text == expected);
    CHECK(provider.state().filter == expected);
    CHECK(session.state().search == expected);
}
