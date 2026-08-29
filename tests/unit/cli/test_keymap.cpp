// keymap 层(交互抛光总账第一步"三层地基"之一)的纯逻辑测试:
//   - 和弦解析/格式化往返:Ctrl+R / alt-v / Shift+Tab / ? / { 这批键位文本
//     怎么写都不丢信息;
//   - platform::KeyInput -> 和弦:专枚举键与带修饰的 Char 各归各位;
//   - 默认表:Composer/Panel/Search 三域的键都在位,固定键入账;
//   - 改绑与冲突:同域撞车拒绝、跨域并存、固定键拒改、坏配置只废单项;
//   - 落盘序列化/解析往返:改绑 -> JSON -> 重新应用,键位一致。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/keymap.hpp"

using namespace lubancode::cli::keymap;

namespace {

KeyChord Ctrl(char32_t ch) {
    KeyChord c;
    c.key = KeyChord::Key::Char;
    c.ch = ch;
    c.ctrl = true;
    return c;
}
KeyChord Plain(char32_t ch) {
    KeyChord c;
    c.key = KeyChord::Key::Char;
    c.ch = ch;
    return c;
}
KeyChord Alt(char32_t ch) {
    KeyChord c;
    c.key = KeyChord::Key::Char;
    c.ch = ch;
    c.alt = true;
    return c;
}
lubancode::platform::KeyInput CharKey(char32_t ch, bool ctrl = false, bool alt = false) {
    lubancode::platform::KeyInput key;
    key.kind = lubancode::platform::KeyInput::Kind::Char;
    key.ch = ch;
    key.ctrl = ctrl;
    key.alt = alt;
    return key;
}

}  // namespace

TEST_CASE("和弦文本:格式化与解析往返") {
    // 解析大小写/分隔符宽容,格式化统一唯一写法。
    CHECK(FormatKeyChord(*ParseKeyChord("ctrl+r")) == "Ctrl+R");
    CHECK(FormatKeyChord(*ParseKeyChord("Ctrl+R")) == "Ctrl+R");
    CHECK(FormatKeyChord(*ParseKeyChord("ALT-V")) == "Alt+V");
    CHECK(FormatKeyChord(*ParseKeyChord("alt+v")) == "Alt+V");
    CHECK(FormatKeyChord(*ParseKeyChord("shift+tab")) == "Shift+Tab");
    CHECK(FormatKeyChord(*ParseKeyChord("?")) == "?");
    CHECK(FormatKeyChord(*ParseKeyChord("{")) == "{");
    CHECK(FormatKeyChord(*ParseKeyChord("x")) == "X");
    CHECK(FormatKeyChord(*ParseKeyChord("enter")) == "Enter");
    CHECK(FormatKeyChord(*ParseKeyChord("pageup")) == "PageUp");
    // Shift+字母折成大写字母,不双写修饰。
    CHECK(FormatKeyChord(*ParseKeyChord("shift+r")) == "R");
    // 认不出:空串、假修饰词、多字符键名。
    CHECK_FALSE(ParseKeyChord("").has_value());
    CHECK_FALSE(ParseKeyChord("hyper+r").has_value());
    CHECK_FALSE(ParseKeyChord("ctrl+notakey").has_value());
    CHECK_FALSE(ParseKeyChord("ctrl+").has_value());
}

TEST_CASE("KeyInput -> 和弦:专枚举键与修饰 Char 各归各位") {
    // platform 的专枚举(CtrlO/CtrlX/CtrlL……)翻成带 ctrl 的 Char 和弦,
    // 反查 /keymap 展示与查表同一份账。
    lubancode::platform::KeyInput ctrl_o;
    ctrl_o.kind = lubancode::platform::KeyInput::Kind::CtrlO;
    const auto chord_o = ChordFromKeyInput(ctrl_o);
    REQUIRE(chord_o.has_value());
    CHECK(chord_o->ctrl);
    CHECK(chord_o->ch == U'o');

    // 带修饰的 Char(platform 新路:Ctrl+S / Alt+V)。
    const auto chord_s = ChordFromKeyInput(CharKey(U's', /*ctrl=*/true));
    REQUIRE(chord_s.has_value());
    CHECK(chord_s->ctrl);
    CHECK(FormatKeyChord(*chord_s) == "Ctrl+S");

    const auto chord_v = ChordFromKeyInput(CharKey(U'V', /*ctrl=*/false, /*alt=*/true));
    REQUIRE(chord_v.has_value());
    CHECK(chord_v->alt);
    CHECK(chord_v->ch == U'v');  // 字母归一小写,大小写入弦不二义
    CHECK(FormatKeyChord(*chord_v) == "Alt+V");

    // 粘贴/NewLine 不是和弦;空事件不是和弦。
    lubancode::platform::KeyInput paste;
    paste.kind = lubancode::platform::KeyInput::Kind::Paste;
    CHECK_FALSE(ChordFromKeyInput(paste).has_value());
    lubancode::platform::KeyInput newline;
    newline.kind = lubancode::platform::KeyInput::Kind::NewLine;
    CHECK_FALSE(ChordFromKeyInput(newline).has_value());
    lubancode::platform::KeyInput none;
    none.kind = lubancode::platform::KeyInput::Kind::None;
    CHECK_FALSE(ChordFromKeyInput(none).has_value());
}

TEST_CASE("默认表:三域键位在位,固定键入账") {
    Keymap map;
    // Composer 域。
    CHECK(map.Lookup(KeyScope::Composer, Ctrl(U'r')) == ActionId::ChatSearchHistory);
    CHECK(map.Lookup(KeyScope::Composer, Ctrl(U'p')) == ActionId::ChatHistoryPrev);
    CHECK(map.Lookup(KeyScope::Composer, Ctrl(U'n')) == ActionId::ChatHistoryNext);
    CHECK(map.Lookup(KeyScope::Composer, Ctrl(U'g')) == ActionId::ChatExternalEditor);
    CHECK(map.Lookup(KeyScope::Composer, Plain(U'?')) == ActionId::HelpShow);
    CHECK(map.Lookup(KeyScope::Composer, Plain(U'{')) == ActionId::TranscriptPrevUserTurn);
    // 固定键入账可查(展示层反查),但 BindableAction 为假。
    CHECK(map.Lookup(KeyScope::Composer, Ctrl(U'o')) == ActionId::TranscriptToggleExpand);
    CHECK(map.Lookup(KeyScope::Composer, Ctrl(U'l')) == ActionId::ScreenRedraw);
    CHECK_FALSE(BindableAction(ActionId::TranscriptToggleExpand));
    CHECK_FALSE(BindableAction(ActionId::ScreenRedraw));
    // stash 无默认键:查不到、ChordFor 为空,但动作名存在。
    CHECK(map.Lookup(KeyScope::Composer, Ctrl(U's')) == ActionId::None);
    CHECK_FALSE(map.ChordFor(ActionId::ComposerStash).has_value());
    CHECK(ActionFromName("composer.stash").has_value());

    // 贴图两键各归各:Alt+V 直贴图,Ctrl+V 智能粘贴(图优先,文本兜底)。
    // platform 两边都把 Ctrl+V 送成 Char 'v'+ctrl,路由必须查得中,否则
    // Ctrl+V 退回死键。
    CHECK(map.Lookup(KeyScope::Composer, Alt(U'v')) == ActionId::ImagePasteClipboard);
    CHECK(map.Lookup(KeyScope::Composer, Ctrl(U'v')) == ActionId::ClipboardSmartPaste);
    CHECK(FormatKeyChord(*map.ChordFor(ActionId::ClipboardSmartPaste)) == "Ctrl+V");
    CHECK(ChordFromKeyInput(CharKey(U'v', /*ctrl=*/true))->key == KeyChord::Key::Char);
    CHECK(ActionFromName("clipboard.smart_paste").has_value());

    // Search 域:Ctrl+R 是"往更早走",与 Composer 的"打开搜索"同键各义。
    CHECK(map.Lookup(KeyScope::Search, Ctrl(U'r')) == ActionId::SearchOlder);
    CHECK(map.Lookup(KeyScope::Search, Ctrl(U's')) == ActionId::SearchScopeCycle);
    CHECK(map.Lookup(KeyScope::Search, Ctrl(U'c')) == ActionId::SearchCancel);
    // Tab 与 Esc 同绑 search.accept(两枚和弦一个动作)。
    KeyChord tab;
    tab.key = KeyChord::Key::Tab;
    KeyChord esc;
    esc.key = KeyChord::Key::Esc;
    CHECK(map.Lookup(KeyScope::Search, tab) == ActionId::SearchAccept);
    CHECK(map.Lookup(KeyScope::Search, esc) == ActionId::SearchAccept);

    // Panel 域。
    KeyChord up;
    up.key = KeyChord::Key::Up;
    CHECK(map.Lookup(KeyScope::Panel, up) == ActionId::AgentNavUp);
    CHECK(map.Lookup(KeyScope::Panel, Ctrl(U'x')) == ActionId::AgentStopAllArm);
    CHECK(map.Lookup(KeyScope::Panel, Ctrl(U'k')) == ActionId::AgentStopAllConfirm);
    CHECK(map.Lookup(KeyScope::Panel, Plain(U'x')) == ActionId::AgentStop);

    // 动作名 <-> ActionId 往返。
    CHECK(ActionFromName(ActionName(ActionId::ChatSearchHistory)) == ActionId::ChatSearchHistory);
    CHECK(ActionFromName("no.such.action") == std::nullopt);

    // 反查:footer/? 帮助从这拿文案。
    CHECK(FormatKeyChord(*map.ChordFor(ActionId::ChatSearchHistory)) == "Ctrl+R");
}

TEST_CASE("改绑与冲突:同域撞车拒绝,跨域并存") {
    Keymap map;
    std::string error;

    // 正常改绑:stash 占上 Ctrl+S(Composer 域)。
    REQUIRE(map.SetBinding(ActionId::ComposerStash, Ctrl(U's'), error));
    CHECK(map.Lookup(KeyScope::Composer, Ctrl(U's')) == ActionId::ComposerStash);

    // 同域冲突:再想把 ChatSearchHistory 也改到 Ctrl+S,拒绝并报撞了谁。
    CHECK_FALSE(map.SetBinding(ActionId::ChatSearchHistory, Ctrl(U's'), error));
    CHECK(error.find("composer.stash") != std::string::npos);

    // 跨域并存:Search 域的 SearchScopeCycle 本来就在 Ctrl+S 上,不受
    // Composer 域那条新绑定影响。
    CHECK(map.Lookup(KeyScope::Search, Ctrl(U's')) == ActionId::SearchScopeCycle);

    // 固定键拒改。
    CHECK_FALSE(map.SetBinding(ActionId::ScreenRedraw, Plain(U'r'), error));
    CHECK_FALSE(map.SetBinding(ActionId::TranscriptToggleExpand, Plain(U'o'), error));

    // 复位:stash 回"无默认",Ctrl+S 在 Composer 域重新查不到。
    REQUIRE(map.ResetBinding(ActionId::ComposerStash, error));
    CHECK(map.Lookup(KeyScope::Composer, Ctrl(U's')) == ActionId::None);
    // 复位一个改过键的可绑动作:回出厂默认。
    REQUIRE(map.SetBinding(ActionId::ChatSearchHistory, Ctrl(U'f'), error));
    REQUIRE(map.ResetBinding(ActionId::ChatSearchHistory, error));
    CHECK(map.Lookup(KeyScope::Composer, Ctrl(U'r')) == ActionId::ChatSearchHistory);
}

TEST_CASE("覆盖落盘:序列化 -> 解析 -> 重新应用,键位一致") {
    Keymap source;
    std::string error;
    REQUIRE(source.SetBinding(ActionId::ChatSearchHistory, Ctrl(U'f'), error));
    const std::string json = source.SerializeOverrides();
    REQUIRE_FALSE(json.empty());

    std::vector<std::string> errors;
    const auto parsed = Keymap::ParseOverridesJson(json, errors);
    REQUIRE(parsed.has_value());
    CHECK(errors.empty());

    Keymap target;  // 出厂默认
    target.ApplyOverrides(*parsed, errors);
    CHECK(errors.empty());
    CHECK(target.Lookup(KeyScope::Composer, Ctrl(U'f')) == ActionId::ChatSearchHistory);
    CHECK(target.Lookup(KeyScope::Composer, Ctrl(U'r')) == ActionId::None);  // 旧键让位

    // 无覆盖时序列化给空串。
    CHECK(Keymap().SerializeOverrides().empty());

    // 坏 JSON 整份忽略;坏条目只废单项。
    CHECK_FALSE(Keymap::ParseOverridesJson("{not json", errors).has_value());
    REQUIRE_FALSE(errors.empty());
    errors.clear();
    const auto partial = Keymap::ParseOverridesJson(
        R"({"no.such.action": "ctrl+q", "chat.search_history": "ctrl+f"})", errors);
    REQUIRE(partial.has_value());
    REQUIRE(partial->size() == 2);
    Keymap mixed;
    mixed.ApplyOverrides(*partial, errors);
    // 坏动作名记一条错,好条目照常生效(坏配置回退并报具体项)。
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("no.such.action") != std::string::npos);
    CHECK(mixed.Lookup(KeyScope::Composer, Ctrl(U'f')) == ActionId::ChatSearchHistory);
}

TEST_CASE("AllBindings:作用域分组齐全,含固定键") {
    Keymap map;
    const auto all = map.AllBindings();
    REQUIRE_FALSE(all.empty());
    bool saw_composer = false;
    bool saw_search = false;
    bool saw_panel = false;
    bool saw_fixed = false;
    bool saw_unbound = false;
    for (const auto& record : all) {
        if (record.scope == KeyScope::Composer && record.action == ActionId::ChatSearchHistory) {
            saw_composer = true;
        }
        if (record.scope == KeyScope::Search && record.action == ActionId::SearchOlder) {
            saw_search = true;
        }
        if (record.scope == KeyScope::Panel && record.action == ActionId::AgentStopAllArm) {
            saw_panel = true;
        }
        if (!record.bindable) {
            saw_fixed = true;
        }
        if (!record.has_default) {
            saw_unbound = true;  // composer.stash 无默认键
        }
    }
    CHECK(saw_composer);
    CHECK(saw_search);
    CHECK(saw_panel);
    CHECK(saw_fixed);
    CHECK(saw_unbound);
}

// ---------------------------------------------------------------------------
// 场景帮助表(`?` 键位帮助只能展开不能收起单):BuildSceneHelpLines 是帮助层
// 的唯一内容源——表头/表尾写 help.show 的实际和弦(改绑跟着改),流式
// 作用域不进表,行数与 AllBindings 对得上。断言只认和弦文本与动作名
// (语言无关),不碰译文的字眼。
// ---------------------------------------------------------------------------

TEST_CASE("场景帮助表:默认绑定下表头表尾写 ?,行数与非流式绑定对齐") {
    const Keymap map;
    const std::vector<std::string> lines = BuildSceneHelpLines(map);
    REQUIRE(lines.size() >= 3);  // 表头 + 至少一行绑定 + 表尾
    // 表头/表尾都带默认和弦(语言无关的断言:只认和弦文本本身)。
    CHECK(lines.front().find("?") != std::string::npos);
    CHECK(lines.back().find("?") != std::string::npos);
    // help.show 自家那行的和弦列也写 ?。
    bool saw_help_show = false;
    std::size_t binding_rows = 0;
    for (std::size_t i = 1; i + 1 < lines.size(); ++i) {
        ++binding_rows;
        CHECK(lines[i].size() >= 12);  // 和弦列定宽 12 + 动作名
        if (lines[i].find("help.show") != std::string::npos) {
            saw_help_show = true;
            CHECK(lines[i].substr(0, 12).find("?") != std::string::npos);
        }
    }
    CHECK(saw_help_show);
    // 行数 = 非 Streaming 绑定数(表头表尾各一行;流式那批不属空闲场景)。
    std::size_t non_streaming = 0;
    for (const auto& record : map.AllBindings()) {
        if (record.scope != KeyScope::Streaming) {
            ++non_streaming;
        }
    }
    CHECK(binding_rows == non_streaming);
    // 固定键也入表(screen.redraw 不可改绑,但可查可显)。
    bool saw_fixed_key = false;
    for (std::size_t i = 1; i + 1 < lines.size(); ++i) {
        if (lines[i].find("screen.redraw") != std::string::npos) {
            saw_fixed_key = true;
        }
    }
    CHECK(saw_fixed_key);
}

TEST_CASE("场景帮助表:改绑 help.show 后表头表尾写新和弦,不再写 ?") {
    Keymap map;
    std::string error;
    REQUIRE(map.SetBinding(ActionId::HelpShow, *ParseKeyChord("alt+h"), error));
    const std::vector<std::string> lines = BuildSceneHelpLines(map);
    REQUIRE(lines.size() >= 3);
    CHECK(lines.front().find("Alt+H") != std::string::npos);
    CHECK(lines.back().find("Alt+H") != std::string::npos);
    CHECK(lines.front().find("?") == std::string::npos);
    CHECK(lines.back().find("?") == std::string::npos);
    // 绑定行里的 help.show 那一行也换成了新和弦。
    for (std::size_t i = 1; i + 1 < lines.size(); ++i) {
        if (lines[i].find("help.show") != std::string::npos) {
            CHECK(lines[i].find("Alt+H") != std::string::npos);
        }
    }
}
