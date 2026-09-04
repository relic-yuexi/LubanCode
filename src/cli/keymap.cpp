// keymap.hpp 的实现:默认表、和弦解析/格式化、冲突检查、用户覆盖的
// 读写。纯逻辑 + 一层薄薄的文件 IO(Load/Save ActiveKeymapOverrides)。

#include "cli/keymap.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

#include <nlohmann/json.hpp>
#include "cli/i18n.hpp"
#include "tools/path_utils.hpp"

namespace lubancode::cli::keymap {

namespace {

// 大小写不敏感的 ASCII 折算(和弦文本解析用;动作名同小写)。
char ToLowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// 和弦文本里的"键名"词 -> 物理键(不含修饰键前缀)。返回 nullopt = 认不出。
std::optional<KeyChord::Key> ParseKeyName(const std::string& word) {
    std::string lower;
    lower.reserve(word.size());
    for (char c : word) {
        lower.push_back(ToLowerAscii(c));
    }
    if (lower == "enter" || lower == "return") return KeyChord::Key::Enter;
    if (lower == "tab") return KeyChord::Key::Tab;
    if (lower == "shifttab" || lower == "shift-tab" || lower == "backtab") return KeyChord::Key::ShiftTab;
    if (lower == "esc" || lower == "escape") return KeyChord::Key::Esc;
    if (lower == "backspace" || lower == "bs") return KeyChord::Key::Backspace;
    if (lower == "delete" || lower == "del") return KeyChord::Key::Delete;
    if (lower == "up") return KeyChord::Key::Up;
    if (lower == "down") return KeyChord::Key::Down;
    if (lower == "left") return KeyChord::Key::Left;
    if (lower == "right") return KeyChord::Key::Right;
    if (lower == "home") return KeyChord::Key::Home;
    if (lower == "end") return KeyChord::Key::End;
    if (lower == "pageup" || lower == "pgup") return KeyChord::Key::PageUp;
    if (lower == "pagedown" || lower == "pgdn" || lower == "pgdown") return KeyChord::Key::PageDown;
    return std::nullopt;
}

std::string KeyName(KeyChord::Key key) {
    switch (key) {
        case KeyChord::Key::Char: return {};
        case KeyChord::Key::Enter: return "Enter";
        case KeyChord::Key::Tab: return "Tab";
        case KeyChord::Key::ShiftTab: return "Shift+Tab";
        case KeyChord::Key::Esc: return "Esc";
        case KeyChord::Key::Backspace: return "Backspace";
        case KeyChord::Key::Delete: return "Delete";
        case KeyChord::Key::Up: return "Up";
        case KeyChord::Key::Down: return "Down";
        case KeyChord::Key::Left: return "Left";
        case KeyChord::Key::Right: return "Right";
        case KeyChord::Key::Home: return "Home";
        case KeyChord::Key::End: return "End";
        case KeyChord::Key::PageUp: return "PageUp";
        case KeyChord::Key::PageDown: return "PageDown";
    }
    return {};
}

}  // namespace

std::string FormatKeyChord(const KeyChord& chord) {
    // Shift+Tab 自带名字,shift 修饰不重复叠加;Char 的 shift 体现在字符本身
    // (大写字母/符号),也不拼 "Shift+"。
    if (chord.key != KeyChord::Key::Char && chord.key != KeyChord::Key::ShiftTab && chord.shift) {
        return "Shift+" + KeyName(chord.key);
    }
    if (chord.key != KeyChord::Key::Char) {
        return KeyName(chord.key);
    }
    std::string out;
    if (chord.ctrl) {
        out += "Ctrl+";
    }
    if (chord.alt) {
        out += "Alt+";
    }
    // 字母统一大写展示(?/{ 这类符号原样)。
    if (chord.ch >= 'a' && chord.ch <= 'z') {
        out.push_back(static_cast<char>(chord.ch - 'a' + 'A'));
    } else if (chord.ch < 0x80) {
        out.push_back(static_cast<char>(chord.ch));
    } else {
        // 非 ASCII 可打印字符(理论上不会出现在绑定表里):按十六进制码点
        // 写出来,至少不丢信息、可往返。
        out += "U+";
        out += std::to_string(static_cast<std::uint32_t>(chord.ch));
    }
    return out;
}

std::optional<KeyChord> ParseKeyChord(std::string_view text) {
    // 剥空白,按 +/- 切段(段内空白也剥),最后一段是键名或单字符。
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == '+' || c == '-') {
            parts.push_back(current);
            current.clear();
        } else if (c != ' ' && c != '\t') {
            current.push_back(ToLowerAscii(c));
        }
    }
    parts.push_back(current);
    if (parts.empty() || parts.back().empty()) {
        return std::nullopt;
    }

    KeyChord chord;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (parts[i].empty()) {
            return std::nullopt;  // "ctrl++x" 这种空段不认
        }
        if (parts[i] == "ctrl" || parts[i] == "control") {
            chord.ctrl = true;
        } else if (parts[i] == "alt" || parts[i] == "option" || parts[i] == "meta") {
            chord.alt = true;
        } else if (parts[i] == "shift") {
            chord.shift = true;
        } else {
            return std::nullopt;  // 认不出的修饰词
        }
    }

    const std::string& last = parts.back();
    if (last.size() == 1) {
        const char c = last[0];
        chord.key = KeyChord::Key::Char;
        // 输入侧字母一律按小写收(格式化统一大写,比较不受大小写影响)。
        chord.ch = static_cast<char32_t>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (chord.shift && chord.ch >= 'a' && chord.ch <= 'z') {
            chord.ch = chord.ch - 'a' + 'A';
            chord.shift = false;  // Shift+r 折成 R,与用户直敲大写一致
        }
        return chord;
    }
    const std::optional<KeyChord::Key> key = ParseKeyName(last);
    if (!key.has_value()) {
        return std::nullopt;
    }
    chord.key = *key;
    if (chord.key == KeyChord::Key::ShiftTab) {
        chord.shift = false;  // 归一:Shift+Tab 不双写
    }
    return chord;
}

std::optional<KeyChord> ChordFromKeyInput(const platform::KeyInput& input) {
    KeyChord chord;
    switch (input.kind) {
        case platform::KeyInput::Kind::None:
        case platform::KeyInput::Kind::Paste:
            return std::nullopt;
        case platform::KeyInput::Kind::Char:
            chord.key = KeyChord::Key::Char;
            chord.ch = input.ch;
            chord.ctrl = input.ctrl;
            chord.alt = input.alt;
            // 键位表里字母一律小写比较,这里归一。
            if (chord.ch >= 'A' && chord.ch <= 'Z') {
                chord.ch = chord.ch - 'A' + 'a';
            }
            break;
        case platform::KeyInput::Kind::Backspace: chord.key = KeyChord::Key::Backspace; break;
        case platform::KeyInput::Kind::Left: chord.key = KeyChord::Key::Left; break;
        case platform::KeyInput::Kind::ShiftLeft: chord.key = KeyChord::Key::Left; chord.shift = true; break;
        case platform::KeyInput::Kind::CtrlLeft: chord.key = KeyChord::Key::Left; chord.ctrl = true; break;
        case platform::KeyInput::Kind::Right: chord.key = KeyChord::Key::Right; break;
        case platform::KeyInput::Kind::Home: chord.key = KeyChord::Key::Home; break;
        case platform::KeyInput::Kind::End: chord.key = KeyChord::Key::End; break;
        case platform::KeyInput::Kind::Up: chord.key = KeyChord::Key::Up; break;
        case platform::KeyInput::Kind::Down: chord.key = KeyChord::Key::Down; break;
        case platform::KeyInput::Kind::Tab: chord.key = KeyChord::Key::Tab; break;
        case platform::KeyInput::Kind::ShiftTab: chord.key = KeyChord::Key::ShiftTab; break;
        case platform::KeyInput::Kind::Enter: chord.key = KeyChord::Key::Enter; break;
        case platform::KeyInput::Kind::NewLine: return std::nullopt;  // 插换行,不当和弦
        case platform::KeyInput::Kind::CtrlC: chord.key = KeyChord::Key::Char; chord.ch = U'c'; chord.ctrl = true; break;
        case platform::KeyInput::Kind::CtrlD: chord.key = KeyChord::Key::Char; chord.ch = U'd'; chord.ctrl = true; break;
        case platform::KeyInput::Kind::CtrlO: chord.key = KeyChord::Key::Char; chord.ch = U'o'; chord.ctrl = true; break;
        case platform::KeyInput::Kind::CtrlE: chord.key = KeyChord::Key::Char; chord.ch = U'e'; chord.ctrl = true; break;
        case platform::KeyInput::Kind::CtrlX: chord.key = KeyChord::Key::Char; chord.ch = U'x'; chord.ctrl = true; break;
        case platform::KeyInput::Kind::CtrlK: chord.key = KeyChord::Key::Char; chord.ch = U'k'; chord.ctrl = true; break;
        case platform::KeyInput::Kind::CtrlL: chord.key = KeyChord::Key::Char; chord.ch = U'l'; chord.ctrl = true; break;
        case platform::KeyInput::Kind::Esc: chord.key = KeyChord::Key::Esc; break;
        case platform::KeyInput::Kind::Delete: chord.key = KeyChord::Key::Delete; break;
        // 其余组合/翻页键暂不映射:保持默认 KeyChord 落空(与改前无 case
        // 时穿出 switch 的行为一致)。
        case platform::KeyInput::Kind::CtrlT:
        case platform::KeyInput::Kind::CtrlP:
        case platform::KeyInput::Kind::CtrlN:
        case platform::KeyInput::Kind::PageUp:
        case platform::KeyInput::Kind::PageDown:
            break;
    }
    return chord;
}

// ---------------------------------------------------------------------------
// 动作名注册表:名字、默认和弦、作用域、可否改绑,一张表说清。
// ---------------------------------------------------------------------------

namespace {

struct ActionInfo {
    ActionId action;
    KeyScope scope;
    const char* name;
    const char* default_chord;  // nullptr = 无默认(动作存在,键位留给用户)
    bool bindable;
};

// 默认绑定表。次序即 /keymap 与 ? 帮助的展示次序(作用域分组)。
// 注意 SearchAccept 有两枚和弦(Tab 与 Esc 同一动作),表里两条记录。
constexpr ActionInfo kActionTable[] = {
    // ---- Composer ----
    {ActionId::ChatSearchHistory, KeyScope::Composer, "chat.search_history", "ctrl+r", true},
    {ActionId::ChatHistoryPrev, KeyScope::Composer, "chat.history_prev", "ctrl+p", true},
    {ActionId::ChatHistoryNext, KeyScope::Composer, "chat.history_next", "ctrl+n", true},
    {ActionId::ChatExternalEditor, KeyScope::Composer, "chat.external_editor", "ctrl+g", true},
    {ActionId::ComposerStash, KeyScope::Composer, "composer.stash", nullptr, true},
    {ActionId::ImagePasteClipboard, KeyScope::Composer, "image.paste_clipboard", "alt+v", true},
    {ActionId::ClipboardSmartPaste, KeyScope::Composer, "clipboard.smart_paste", "ctrl+v", true},
    {ActionId::HelpShow, KeyScope::Composer, "help.show", "?", true},
    {ActionId::TranscriptPrevUserTurn, KeyScope::Composer, "transcript.prev_user_turn", "{", true},
    {ActionId::TranscriptNextUserTurn, KeyScope::Composer, "transcript.next_user_turn", "}", true},
    {ActionId::TranscriptToScrollback, KeyScope::Composer, "transcript.to_scrollback", "[", true},
    {ActionId::TranscriptViewInEditor, KeyScope::Composer, "transcript.view_in_editor", "v", true},
    // 既有固定键:入账可查可显(footer/? 帮助反查),不改绑——它们嵌在
    // 编辑器核心与流式监听的安全路径里,不是一层皮。
    {ActionId::TranscriptToggleExpand, KeyScope::Composer, "transcript.toggle_expand", "ctrl+o", false},
    {ActionId::TranscriptFocusView, KeyScope::Composer, "transcript.focus_view", "ctrl+e", false},
    {ActionId::ScreenRedraw, KeyScope::Composer, "screen.redraw", "ctrl+l", false},
    {ActionId::TranscriptToggleExpand, KeyScope::Streaming, "transcript.toggle_expand", "ctrl+o", false},
    // ---- Search ----
    {ActionId::SearchOlder, KeyScope::Search, "search.older", "ctrl+r", true},
    {ActionId::SearchScopeCycle, KeyScope::Search, "search.scope_cycle", "ctrl+s", true},
    {ActionId::SearchAccept, KeyScope::Search, "search.accept", "tab", true},
    {ActionId::SearchAccept, KeyScope::Search, "search.accept", "esc", true},
    {ActionId::SearchAcceptSubmit, KeyScope::Search, "search.accept_submit", "enter", true},
    {ActionId::SearchCancel, KeyScope::Search, "search.cancel", "ctrl+c", true},
    // ---- Panel ----
    {ActionId::AgentNavUp, KeyScope::Panel, "agent.nav_up", "up", true},
    {ActionId::AgentNavDown, KeyScope::Panel, "agent.nav_down", "down", true},
    {ActionId::AgentView, KeyScope::Panel, "agent.view", "enter", true},
    {ActionId::AgentBack, KeyScope::Panel, "agent.back", "esc", true},
    {ActionId::AgentStop, KeyScope::Panel, "agent.stop", "x", true},
    {ActionId::AgentStopAllArm, KeyScope::Panel, "agent.stop_all_arm", "ctrl+x", true},
    {ActionId::AgentStopAllConfirm, KeyScope::Panel, "agent.stop_all_confirm", "ctrl+k", true},
};

constexpr std::size_t kActionCount = std::size(kActionTable);

}  // namespace

const char* ActionName(ActionId action) {
    for (const auto& info : kActionTable) {
        if (info.action == action) {
            return info.name;
        }
    }
    return "";
}

std::optional<ActionId> ActionFromName(std::string_view name) {
    for (const auto& info : kActionTable) {
        if (name == info.name) {
            return info.action;
        }
    }
    return std::nullopt;
}

const char* ScopeName(KeyScope scope) {
    switch (scope) {
        case KeyScope::Composer: return "composer";
        case KeyScope::Search: return "search";
        case KeyScope::Panel: return "panel";
        case KeyScope::Streaming: return "streaming";
    }
    return "";
}

bool BindableAction(ActionId action) {
    for (const auto& info : kActionTable) {
        if (info.action == action) {
            return info.bindable;
        }
    }
    return false;
}

std::vector<std::string> BuildSceneHelpLines(const Keymap& keymap) {
    // 收起出口写实际和弦:改绑后表头/表尾跟着改,不硬写问号;未绑键时
    // 拿动作名兜底,起码指得出 /keymap set help.show <和弦> 这条路。
    std::string chord_label;
    if (const std::optional<KeyChord> chord = keymap.ChordFor(ActionId::HelpShow);
        chord.has_value()) {
        chord_label = FormatKeyChord(*chord);
    } else {
        chord_label = ActionName(ActionId::HelpShow);
    }

    std::vector<std::string> lines;
    lines.push_back(trf("help.scene_header", chord_label));
    constexpr int kChordColumnWidth = 12;  // 和弦列宽:最长 "Ctrl+X Ctrl+K" 一档
    for (const BindingRecord& record : keymap.AllBindings()) {
        if (record.scope == KeyScope::Streaming) {
            continue;  // 流式脚注那批不属"当前场景"(空闲 composer)
        }
        std::string row = record.has_default ? FormatKeyChord(record.chord) : "-";
        for (int pad = static_cast<int>(row.size()); pad < kChordColumnWidth; ++pad) {
            row.push_back(' ');
        }
        row += ActionName(record.action);
        if (!record.bindable) {
            row += tr("help.fixed_suffix");
        } else if (!record.has_default) {
            row += tr("help.unbound_suffix");
        }
        lines.push_back(std::move(row));
    }
    lines.push_back(trf("help.scene_footer", chord_label));
    return lines;
}

std::string BuildKeyHints(const Keymap& keymap,
                          const std::vector<std::pair<ActionId, const char*>>& actions) {
    std::string hints;
    for (const auto& [action, label_key] : actions) {
        const auto chord = keymap.ChordFor(action);
        if (!chord.has_value()) {
            continue;  // 未绑定:整段略过,不留下孤单分隔符
        }
        if (!hints.empty()) {
            hints += " · ";
        }
        hints += FormatKeyChord(*chord) + " " + tr(label_key);
    }
    return hints;
}

Keymap::Keymap() {
    entries_.reserve(kActionCount);
    for (const auto& info : kActionTable) {
        Entry entry;
        entry.action = info.action;
        entry.scope = info.scope;
        entry.bindable = info.bindable;
        entry.has_default = info.default_chord != nullptr;
        if (entry.has_default) {
            entry.default_chord = *ParseKeyChord(info.default_chord);
            entry.chord = entry.default_chord;
        }
        entries_.push_back(entry);
    }
}

ActionId Keymap::Lookup(KeyScope scope, const KeyChord& chord) const {
    for (const auto& entry : entries_) {
        if (entry.scope != scope || !entry.has_default) {
            continue;  // 别的作用域 / 没绑键的动作查不到
        }
        if (entry.chord == chord) {
            return entry.action;
        }
    }
    return ActionId::None;
}

std::optional<KeyChord> Keymap::ChordFor(ActionId action) const {
    for (const auto& entry : entries_) {
        if (entry.action == action && entry.has_default) {
            return entry.chord;
        }
    }
    return std::nullopt;
}

bool Keymap::SetBinding(ActionId action, KeyChord chord, std::string& error) {
    if (!BindableAction(action)) {
        error = ActionName(action);
        error += ": 固定键,不可改绑";
        return false;
    }
    // 冲突检查:同作用域里,这枚和弦若已绑在"别的动作"上就拒绝。同一
    // 动作的两枚和弦(SearchAccept 的 Tab/Esc)互不挡道——只比对 action
    // 不同的记录;跨作用域各归各(Composer 的 Ctrl+R 与 Search 的 Ctrl+R
    // 可以并存)。
    for (const auto& entry : entries_) {
        if (entry.has_default && entry.chord == chord && entry.action != action) {
            // 作用域相同的才冲突:同一动作允许多枚和弦,跨作用域各归各。
            // 找出目标动作自己的(任一)作用域来比。
            bool same_scope = false;
            for (const auto& mine : entries_) {
                if (mine.action == action && mine.scope == entry.scope) {
                    same_scope = true;
                    break;
                }
            }
            if (same_scope) {
                error = FormatKeyChord(chord);
                error += " 已被 ";
                error += ActionName(entry.action);
                error += " 占用(作用域 ";
                error += ScopeName(entry.scope);
                error += ")";
                return false;
            }
        }
    }
    bool applied = false;
    for (auto& entry : entries_) {
        if (entry.action == action && entry.bindable) {
            // 同一动作多枚和弦(Tab/Esc):两条记录一并换到新和弦上,
            // 不许一条 Tab 一条 Esc 各奔东西(冲突检查已保证不撞别人)。
            entry.chord = chord;
            entry.has_default = true;
            applied = true;
        }
    }
    if (!applied) {
        error = "未知动作";
        return false;
    }
    return true;
}

bool Keymap::ResetBinding(ActionId action, std::string& error) {
    if (!BindableAction(action)) {
        error = ActionName(action);
        error += ": 固定键,不可复位";
        return false;
    }
    bool applied = false;
    for (auto& entry : entries_) {
        if (entry.action == action) {
            entry.chord = entry.default_chord;
            entry.has_default = true;
            applied = true;
        }
    }
    if (!applied) {
        error = "未知动作";
        return false;
    }
    return true;
}

std::vector<BindingRecord> Keymap::AllBindings() const {
    std::vector<BindingRecord> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        BindingRecord record;
        record.action = entry.action;
        record.scope = entry.scope;
        record.chord = entry.chord;
        record.default_chord = entry.default_chord;
        record.bindable = entry.bindable;
        record.has_default = entry.has_default;
        out.push_back(record);
    }
    // 展示次序:作用域(枚举序)分组,名字典序。
    std::sort(out.begin(), out.end(), [](const BindingRecord& a, const BindingRecord& b) {
        if (a.scope != b.scope) {
            return static_cast<int>(a.scope) < static_cast<int>(b.scope);
        }
        return std::string_view(ActionName(a.action)) < std::string_view(ActionName(b.action));
    });
    return out;
}

std::optional<std::vector<std::pair<std::string, std::string>>> Keymap::ParseOverridesJson(
    const std::string& json_text, std::vector<std::string>& errors) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(json_text);
    } catch (const std::exception& e) {
        errors.push_back(std::string("keymap.json: JSON 解析失败(") + e.what() + "),整份忽略");
        return std::nullopt;
    }
    if (!parsed.is_object()) {
        errors.push_back("keymap.json: 顶层不是对象,整份忽略");
        return std::nullopt;
    }
    std::vector<std::pair<std::string, std::string>> out;
    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
        if (!it.value().is_string()) {
            errors.push_back("keymap.json: " + it.key() + " 的值不是字符串,跳过");
            continue;
        }
        out.emplace_back(it.key(), it.value().get<std::string>());
    }
    return out;
}

void Keymap::ApplyOverrides(const std::vector<std::pair<std::string, std::string>>& overrides,
                            std::vector<std::string>& errors) {
    for (const auto& [name, chord_text] : overrides) {
        const auto action = ActionFromName(name);
        if (!action.has_value()) {
            errors.push_back(name + ": 不认得这个动作名,跳过");
            continue;
        }
        const auto chord = ParseKeyChord(chord_text);
        if (!chord.has_value()) {
            errors.push_back(name + ": 和弦 \"" + chord_text + "\" 解析不动,跳过");
            continue;
        }
        std::string error;
        if (!SetBinding(*action, *chord, error)) {
            errors.push_back(name + ": " + error);
        }
    }
}

std::string Keymap::SerializeOverrides() const {
    // 只写与默认不同的项;同动作多枚和弦只写第一枚。保持插入次序稳定
    // (map 按动作名排序,写盘可重现)。
    std::map<std::string, std::string> changed;
    for (const auto& entry : entries_) {
        if (!entry.has_default || !(entry.chord != entry.default_chord)) {
            continue;
        }
        const std::string name = ActionName(entry.action);
        if (!changed.contains(name)) {
            changed[name] = FormatKeyChord(entry.chord);
        }
    }
    if (changed.empty()) {
        return {};
    }
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [name, chord] : changed) {
        out[name] = chord;
    }
    return out.dump(2) + "\n";
}

// ---------------------------------------------------------------------------
// 进程级活动表
// ---------------------------------------------------------------------------

Keymap& ActiveKeymap() {
    static Keymap active;
    return active;
}

std::string KeymapOverridesPath(const std::string& user_lubancode_dir) {
    if (user_lubancode_dir.empty()) {
        return {};
    }
    return user_lubancode_dir + "/keymap.json";
}

std::vector<std::string> LoadActiveKeymapOverrides(const std::string& user_lubancode_dir) {
    std::vector<std::string> warnings;
    const std::string path = KeymapOverridesPath(user_lubancode_dir);
    if (path.empty()) {
        return warnings;
    }
    std::error_code ec;
    if (!std::filesystem::exists(lubancode::tools::Utf8ToPath(path), ec)) {
        return warnings;  // 没这文件 = 无覆盖,不算错
    }
    std::ifstream in(lubancode::tools::Utf8ToPath(path), std::ios::binary);
    if (!in) {
        warnings.push_back("keymap.json: 打不开,沿用默认键位");
        return warnings;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const auto overrides = Keymap::ParseOverridesJson(buffer.str(), warnings);
    if (overrides.has_value()) {
        ActiveKeymap().ApplyOverrides(*overrides, warnings);
    }
    return warnings;
}

std::optional<std::string> SaveActiveKeymapOverrides(const std::string& user_lubancode_dir) {
    const std::string path = KeymapOverridesPath(user_lubancode_dir);
    if (path.empty()) {
        return std::string("没有用户目录,键位没落盘");
    }
    const std::string text = ActiveKeymap().SerializeOverrides();
    const auto fs_path = lubancode::tools::Utf8ToPath(path);
    std::error_code ec;
    if (text.empty()) {
        // 覆盖全复位后文件里已无差异:删掉文件,免得留一份空壳。
        if (std::filesystem::exists(fs_path, ec)) {
            std::filesystem::remove(fs_path, ec);
            if (ec) {
                return std::string("删 keymap.json 失败: ") + ec.message();
            }
        }
        return std::nullopt;
    }
    std::ofstream out(fs_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::string("keymap.json 写不进去(目录不存在或没权限)");
    }
    out << text;
    out.flush();
    if (!out) {
        return std::string("keymap.json 写入中断,键位可能没保存全");
    }
    return std::nullopt;
}

}  // namespace lubancode::cli::keymap
