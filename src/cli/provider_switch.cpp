// /provider switch 选择器:纯逻辑(条目/过滤/按键状态机)+ TTY 终端面板。
// 接口见 provider_switch.hpp 文件头。

#include "cli/provider_switch.hpp"

#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

#include <algorithm>
#include <iostream>
#include <mutex>

#include "cli/console_input.hpp"
#include "cli/divider.hpp"  // divider::line:面板横线收口(排版批 6)
#include "cli/i18n.hpp"
#include "cli/line_editor.hpp"
#include "cli/menu_text_edit.hpp"  // AppendUtf8/EraseLastUtf8:三菜单共用的文本槽内核(HC-04)
#include "cli/panel_chrome.hpp"    // PanelRowTone:行级语义档(排版批 6)
#include "platform/console.hpp"

namespace lubancode::cli {

namespace {

std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// platform::KeyInput -> cli::KeyEvent 的本地映射(本面板用得上的那几个键)。
// console_input.cpp 的 MapKey 是它的私货,这里不越界去引用。
std::optional<KeyEvent> MapSwitchKey(const platform::KeyInput& key) {
    using PK = platform::KeyInput::Kind;
    switch (key.kind) {
        case PK::Char:
            return KeyEvent::Char(key.ch);
        case PK::Paste:
            return KeyEvent::Paste(key.text);
        case PK::Backspace:
            return KeyEvent::Simple(KeyKind::Backspace);
        case PK::Up:
            return KeyEvent::Simple(KeyKind::Up);
        case PK::Down:
            return KeyEvent::Simple(KeyKind::Down);
        case PK::Home:
            return KeyEvent::Simple(KeyKind::Home);
        case PK::End:
            return KeyEvent::Simple(KeyKind::End);
        case PK::Enter:
            return KeyEvent::Simple(KeyKind::Enter);
        case PK::NewLine:
            return KeyEvent::Simple(KeyKind::NewLine);
        case PK::Esc:
            return KeyEvent::Simple(KeyKind::Esc);
        case PK::CtrlC:
            return KeyEvent::Simple(KeyKind::CtrlC);
        case PK::CtrlD:
            return KeyEvent::Simple(KeyKind::CtrlD);
        case PK::ShiftTab:
            return KeyEvent::Simple(KeyKind::ShiftTab);
        case PK::Tab:
            return KeyEvent::Simple(KeyKind::Tab);
        case PK::None:
        default:
            return std::nullopt;
    }
}

}  // namespace

std::string ShortenProviderUrl(const std::string& base_url) {
    std::size_t begin = base_url.find("://");
    begin = begin == std::string::npos ? 0 : begin + 3;
    std::string rest = base_url.substr(begin);
    // 只留主机[:端口]与至多一段路径(v1 这类),长路径不往列表里搬。
    const std::size_t first_slash = rest.find('/');
    if (first_slash != std::string::npos) {
        const std::size_t second_slash = rest.find('/', first_slash + 1);
        rest = rest.substr(0, second_slash == std::string::npos ? std::string::npos : second_slash);
    }
    while (!rest.empty() && rest.back() == '/') {
        rest.pop_back();
    }
    return rest;
}

std::vector<ProviderSwitchEntry> BuildProviderSwitchEntries(
    const std::vector<config::ProviderConfig>& providers, const std::string& active_provider) {
    std::vector<ProviderSwitchEntry> entries;
    entries.reserve(providers.size());
    for (const auto& provider : providers) {
        ProviderSwitchEntry entry;
        entry.name = provider.name;
        entry.model = provider.model.empty() ? std::string(tr("cmd.provider.model_unset")) : provider.model;
        entry.short_url = ShortenProviderUrl(provider.base_url);
        entry.is_current = provider.name == active_provider;
        const config::ProviderAuthResolution auth = config::ResolveProviderAuth(provider);
        switch (auth.status) {
            case config::ProviderAuthResolution::Status::NotRequired:
                entry.auth_label = tr("cmd.provider.auth_none");
                break;
            case config::ProviderAuthResolution::Status::Ready:
                entry.auth_label = tr("provider_switch.auth_ready");
                break;
            case config::ProviderAuthResolution::Status::Missing:
                if (provider.auth == config::ProviderAuthMode::Inline) {
                    entry.auth_label = tr("provider_switch.auth_inline_missing");
                } else {
                    entry.auth_label = trf("provider_switch.auth_env_missing", auth.env_name);
                }
                break;
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::vector<std::size_t> FilterProviderSwitchEntries(const std::vector<ProviderSwitchEntry>& entries,
                                                     const std::string& filter) {
    std::vector<std::size_t> out;
    if (filter.empty()) {
        out.reserve(entries.size());
        for (std::size_t i = 0; i < entries.size(); ++i) {
            out.push_back(i);
        }
        return out;
    }
    const std::string needle = ToLowerAscii(filter);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string haystack =
            ToLowerAscii(entries[i].name + " " + entries[i].model + " " + entries[i].short_url);
        if (haystack.find(needle) != std::string::npos) {
            out.push_back(i);
        }
    }
    return out;
}

ProviderSwitchCore::ProviderSwitchCore(std::size_t visible_count, std::size_t initial_cursor)
    : visible_count_(visible_count) {
    state_.cursor = visible_count_ > 0 ? (initial_cursor < visible_count_ ? initial_cursor : 0) : 0;
}

void ProviderSwitchCore::SetVisibleCount(std::size_t visible_count) {
    visible_count_ = visible_count;
    if (state_.cursor >= visible_count_) {
        state_.cursor = visible_count_ > 0 ? visible_count_ - 1 : 0;
    }
}

const ProviderSwitchCore::State& ProviderSwitchCore::HandleKey(const KeyEvent& event) {
    if (state_.submitted || state_.cancelled) {
        return state_;
    }
    switch (event.kind) {
        case KeyKind::Up:
        case KeyKind::ShiftTab:
            if (visible_count_ > 0) {
                state_.cursor = state_.cursor == 0 ? visible_count_ - 1 : state_.cursor - 1;
            }
            break;
        case KeyKind::Down:
        case KeyKind::Tab:
            if (visible_count_ > 0) {
                state_.cursor = (state_.cursor + 1) % visible_count_;
            }
            break;
        case KeyKind::Home:
            state_.cursor = 0;
            break;
        case KeyKind::End:
            if (visible_count_ > 0) {
                state_.cursor = visible_count_ - 1;
            }
            break;
        case KeyKind::Char:
            if (event.ch >= 0x20 && event.ch != 0x7F) {
                AppendUtf8(state_.filter, event.ch);
            }
            break;
        case KeyKind::Paste:
            for (const char c : event.text) {
                if (c != '\r' && c != '\n' && c != '\0') {
                    state_.filter.push_back(c);
                }
            }
            break;
        case KeyKind::Backspace:
            EraseLastUtf8(state_.filter);
            break;
        case KeyKind::Enter:
        case KeyKind::NewLine:
            state_.submitted = visible_count_ > 0;
            break;
        case KeyKind::Esc:
        case KeyKind::CtrlC:
        case KeyKind::CtrlD:
            state_.cancelled = true;
            break;
        default:
            break;
    }
    return state_;
}

// ---------------------------------------------------------------------------
// TTY 面板:列表 + 筛选行 + footer,原地重画。
// ---------------------------------------------------------------------------

namespace {

// 空列表时的两个固定选项:添加 provider / 取消。
constexpr std::size_t kEmptyAddIndex = 0;
constexpr std::size_t kEmptyCancelIndex = 1;

// 面板上下横线(排版批 6 收口):字符走 divider::line(Light 档;plain 退
// Ascii),颜色 frame_border 档——与 cli::frame::* 边框、底栏 BoxRuleLine
// 同一语义。宽度口径不变(width-2,面板左右各让一格)。
std::string PanelRule(const Theme& theme, int width) {
    const int n = width > 4 ? width - 2 : 20;
    const bool plain = theme.reset.empty();
    return theme.frame_border +
           divider::line(plain ? divider::Style::Ascii : divider::Style::Light, n) + theme.reset;
}

}  // namespace

ProviderSwitchResult RunProviderSwitchPicker(const std::vector<config::ProviderConfig>& providers,
                                              const std::string& active_provider,
                                              const std::string& start_filter, const std::string& notice,
                                              const std::string& start_cursor_name, const Theme& theme,
                                              bool edit_on_enter) {
    ProviderSwitchResult result;
    if (!platform::StdinIsInteractive() || !platform::ProbeStdoutConsole().is_console ||
        !platform::SupportsScreenRepaint()) {
        return result;  // 面板开不了:调用方按非 TTY 规矩给短用法
    }

    const bool empty_list = providers.empty();
    const std::vector<ProviderSwitchEntry> entries =
        empty_list ? std::vector<ProviderSwitchEntry>{}
                   : BuildProviderSwitchEntries(providers, active_provider);
    std::vector<std::size_t> visible =
        empty_list ? std::vector<std::size_t>{} : FilterProviderSwitchEntries(entries, start_filter);
    // 空列表/筛空:固定两项(添加 provider / 取消)。
    const std::size_t initial_choice_count = visible.empty() ? 2 : visible.size();
    // 光标先停在指定名字(补钥页回列表还原选择),否则当前 provider,再退首项。
    std::size_t initial_cursor = 0;
    for (std::size_t i = 0; i < visible.size(); ++i) {
        if (!start_cursor_name.empty() && entries[visible[i]].name == start_cursor_name) {
            initial_cursor = i;
            break;
        }
        if (start_cursor_name.empty() && entries[visible[i]].is_current) {
            initial_cursor = i;
            break;
        }
    }

    std::lock_guard<std::recursive_timed_mutex> console_read_lock(ConsoleReadMutex());
    platform::RawInputScope raw_scope;
    if (!raw_scope.ok()) {
        return result;
    }

    ProviderSwitchCore core(initial_choice_count, initial_cursor);
    core.SetFilter(start_filter);  // 进来就带筛选词

    int start_row = 0;
    int rows_drawn = 0;
    int width = 80;
    auto draw = [&]() {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (!info.has_value()) {
            return false;
        }
        width = info->width > 8 ? info->width : 80;
        const bool fixed_pair = visible.empty();  // 空列表或筛空:添加/取消两项

        std::vector<std::string> lines;
        // 行档并行账(排版批 6):横线自带色(PanelRule),其余行按结构角色
        // 取语义档,画的时候统一查表;焦点("> " 前缀)仍是 confirm,交互
        // 语义一字不动。
        std::vector<PanelRowTone> tones;
        const auto push_line = [&](std::string line, PanelRowTone tone) {
            lines.push_back(std::move(line));
            tones.push_back(tone);
        };
        lines.push_back(PanelRule(theme, width));
        tones.push_back(PanelRowTone::Body);  // 横线自带色,画时不再包
        push_line(tr(edit_on_enter ? "provider_switch.edit_title" : "provider_switch.title"),
                  PanelRowTone::Title);
        push_line("", PanelRowTone::Body);
        if (!notice.empty()) {
            push_line(TruncateUtf8ToDisplayWidth("! " + notice, width - 2), PanelRowTone::Warning);
        }
        if (fixed_pair) {
            push_line(empty_list ? tr("provider_switch.empty_hint") : tr("provider_switch.no_match_hint"),
                      PanelRowTone::Muted);
            push_line((core.state().cursor == kEmptyAddIndex ? "> " : "  ") +
                          std::string(tr("provider_switch.opt_add")),
                      PanelRowTone::Body);
            push_line((core.state().cursor == kEmptyCancelIndex ? "> " : "  ") +
                          std::string(tr("provider_switch.opt_cancel")),
                      PanelRowTone::Body);
        } else {
            for (std::size_t i = 0; i < visible.size(); ++i) {
                const ProviderSwitchEntry& entry = entries[visible[i]];
                std::string line = (i == core.state().cursor ? "> " : "  ");
                line += entry.name + "  " + entry.model + "  " + entry.short_url + "  " + entry.auth_label;
                if (entry.is_current) {
                    line += "  " + std::string(tr("cmd.provider.current"));
                }
                push_line(TruncateUtf8ToDisplayWidth(line, width - 2), PanelRowTone::Body);
            }
        }
        push_line("", PanelRowTone::Body);
        push_line(trf("provider_switch.filter_line",
                      core.state().filter.empty()
                          ? std::string(tr("provider_switch.filter_empty"))
                          : core.state().filter),
                  PanelRowTone::Muted);
        push_line(tr(edit_on_enter ? "provider_switch.footer_edit" : "provider_switch.footer"),
                  PanelRowTone::Hint);
        lines.push_back(PanelRule(theme, width));
        tones.push_back(PanelRowTone::Body);

        const int rows_needed = static_cast<int>(lines.size()) + 2;
        // 首帧从当前光标起画。后续重画先回旧帧顶再核空间；若从上一帧
        // 末尾探底，每按一键都会在旧帧下面另起一块，旧列表便留在屏上。
        if (rows_drawn > 0) {
            platform::SetCursorPos(0, start_row);
        }
        if (!EnsureStreamScreenRowsLocked(rows_needed)) {
            return false;
        }
        const std::optional<platform::ScreenInfo> after = platform::GetScreenInfo();
        if (!after.has_value()) {
            return false;
        }
        // Ensure... 若贴底滚了内容，会把光标拨回滚后的真实位置；没滚时
        // 仍在旧 start_row。两种情形都拿它作新帧顶，原地清旧画新。
        start_row = after->cursor_y;
        const int rows_to_draw = static_cast<int>(lines.size());
        const int clear_rows = (std::max)(rows_drawn, rows_to_draw);
        TermOut() << "\x1b[?2026h\x1b[?25l";
        for (int r = 0; r < clear_rows; ++r) {
            platform::ClearRowHardFrom(0, start_row + r, width);
        }
        for (int r = 0; r < rows_to_draw; ++r) {
            platform::SetCursorPos(0, start_row + r);
            const std::string& line = lines[static_cast<std::size_t>(r)];
            if (line.rfind("> ", 0) == 0 && !theme.confirm.empty()) {
                TermOut() << theme.confirm << line << theme.reset;  // 光标行上色
            } else {
                const std::string& color =
                    PanelRowToneColor(theme, tones[static_cast<std::size_t>(r)]);
                if (!line.empty() && !color.empty()) {
                    TermOut() << color << line << theme.reset;
                } else {
                    TermOut() << line;
                }
            }
        }
        rows_drawn = rows_to_draw;
        TermOut() << "\x1b[?2026l\x1b[?25h";
        platform::SetCursorPos(0, start_row + rows_to_draw);
        TermOut().flush();
        return true;
    };

    auto clear = [&] {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (info.has_value()) {
            for (int r = 0; r < rows_drawn; ++r) {
                platform::ClearRowHardFrom(0, start_row + r, info->width);
            }
            platform::SetCursorPos(0, start_row);
        }
        rows_drawn = 0;
    };

    if (!draw()) {
        return result;
    }
    platform::KeyReader key_reader;
    while (!core.state().submitted && !core.state().cancelled) {
        const std::optional<platform::KeyInput> raw_key = key_reader.ReadOne();
        if (!raw_key.has_value()) {
            clear();
            return result;  // EOF:取消,不改任何东西
        }
        const std::optional<KeyEvent> mapped = MapSwitchKey(*raw_key);
        if (!mapped.has_value()) {
            continue;
        }
        // 快捷键 e(容错单):筛选词为空、列表里有高亮项时,直接进该家的
        // 编辑向导。非空筛选词里 e 仍是普通字符——不然"deepseek"这类词
        // 根本没法往筛选里敲。
        if (mapped->kind == KeyKind::Char && mapped->ch == U'e' && core.state().filter.empty() &&
            !visible.empty()) {
            clear();
            result.pick = ProviderSwitchPick::Edit;
            result.name = entries[visible[core.state().cursor]].name;
            return result;
        }
        const std::string filter_before = core.state().filter;
        core.HandleKey(*mapped);
        if (core.state().filter != filter_before) {
            visible = FilterProviderSwitchEntries(entries, core.state().filter);
            core.SetVisibleCount(visible.empty() ? 2 : visible.size());
        }
        if (!core.state().submitted && !core.state().cancelled) {
            if (!draw()) {
                clear();
                return result;
            }
        }
    }

    clear();
    result.filter = core.state().filter;  // 筛选词随结果带出(补钥页回列表还原)
    if (core.state().cancelled) {
        return result;
    }
    const std::size_t cursor = core.state().cursor;
    if (visible.empty()) {
        // 空列表/筛空的固定两项:添加/取消。
        result.pick = cursor == kEmptyAddIndex ? ProviderSwitchPick::AddNew : ProviderSwitchPick::Cancelled;
        return result;
    }
    if (cursor >= visible.size()) {
        return result;
    }
    result.pick = ProviderSwitchPick::Named;
    result.name = entries[visible[cursor]].name;
    return result;
}

}  // namespace lubancode::cli
