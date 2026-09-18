// provider_panel_core.hpp 的纯逻辑实现:条目生成(§名字/当前标记/鉴权
// 缺失短标记)、长列表按键状态机(上下移动 + 滚动窗口)与帧拼装。不碰
// 终端、不碰配置文件;TTY 宿主在 provider_panel.cpp(归 lubancode_app)。
#include "cli/provider_panel_core.hpp"

#include <algorithm>
#include <utility>

#include "cli/i18n.hpp"
#include "cli/line_editor.hpp"  // TruncateUtf8ToDisplayWidth(窄终端截行)

namespace lubancode::cli {

std::vector<ProviderPanelEntry> BuildProviderPanelEntries(
    const std::vector<config::ProviderConfig>& providers, const std::string& active_provider) {
    std::vector<ProviderPanelEntry> entries;
    entries.reserve(providers.size());
    for (const config::ProviderConfig& provider : providers) {
        ProviderPanelEntry entry;
        entry.name = provider.name;
        entry.is_current = provider.name == active_provider;
        // 鉴权三态只认 Missing:给一枚短标记提示"这一家缺钥匙",选中后
        // 补救页(provider_remedy)才讲细账;Ready/NotRequired 不打标。
        if (config::ResolveProviderAuth(provider).status ==
            config::ProviderAuthResolution::Status::Missing) {
            entry.auth_note = tr("provider_panel.auth_missing");
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::size_t FindProviderPanelIndex(const std::vector<ProviderPanelEntry>& entries,
                                   const std::string& name) {
    if (name.empty()) {
        return static_cast<std::size_t>(-1);  // 空名不给落点,调用方落到首项
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name == name) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

// ---------------------------------------------------------------------------
// 按键状态机
// ---------------------------------------------------------------------------

ProviderPanelCore::ProviderPanelCore(std::size_t entry_count, std::size_t visible_capacity,
                                     std::size_t start_index)
    : entry_count_(entry_count), capacity_(visible_capacity == 0 ? 1 : visible_capacity) {
    if (entry_count_ > 0) {
        state_.cursor = start_index < entry_count_ ? start_index : 0;  // 越界钳回首项
    }
    ClampWindow();
}

void ProviderPanelCore::ClampWindow() {
    if (state_.cursor < state_.offset) {
        state_.offset = state_.cursor;
    } else if (state_.cursor >= state_.offset + capacity_) {
        state_.offset = state_.cursor - capacity_ + 1;
    }
}

const ProviderPanelCore::State& ProviderPanelCore::HandleKey(const KeyEvent& event) {
    if (state_.submitted || state_.cancelled) {
        return state_;  // 收场后键一律落空
    }
    switch (event.kind) {
        case KeyKind::Up:
        case KeyKind::ShiftTab:
            if (entry_count_ > 0) {
                state_.cursor = state_.cursor == 0 ? entry_count_ - 1 : state_.cursor - 1;
            }
            break;
        case KeyKind::Down:
        case KeyKind::Tab:
            if (entry_count_ > 0) {
                state_.cursor = (state_.cursor + 1) % entry_count_;
            }
            break;
        case KeyKind::Home:
            if (entry_count_ > 0) {
                state_.cursor = 0;
            }
            break;
        case KeyKind::End:
            if (entry_count_ > 0) {
                state_.cursor = entry_count_ - 1;
            }
            break;
        case KeyKind::PageUp:
            // 翻页不环绕(与 Home/End 一样是绝对动作),到头钳住。
            if (entry_count_ > 0) {
                state_.cursor = state_.cursor >= capacity_ ? state_.cursor - capacity_ : 0;
            }
            break;
        case KeyKind::PageDown:
            if (entry_count_ > 0) {
                state_.cursor = state_.cursor + capacity_ >= entry_count_
                                    ? entry_count_ - 1
                                    : state_.cursor + capacity_;
            }
            break;
        case KeyKind::Enter:
        case KeyKind::NewLine:
            state_.submitted = entry_count_ > 0;  // 空清单没有可选项,不算选中
            break;
        case KeyKind::Esc:
        case KeyKind::CtrlC:
        case KeyKind::CtrlD:
            // Ctrl+C 按面板取消处理(与 context_window 面板同款),不退程序。
            state_.cancelled = true;
            break;
        default:
            break;  // 字符/粘贴/退格一概不收:这块面板只挑不筛
    }
    ClampWindow();
    return state_;
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

ProviderPanelFrame BuildProviderPanelFrame(const ProviderPanelView& view, int width) {
    ProviderPanelFrame frame;
    const int usable = width > 4 ? width - 2 : 40;
    const auto push = [&frame, usable](std::string line) {
        frame.lines.push_back(TruncateUtf8ToDisplayWidth(std::move(line), usable));
    };

    // 标题 + 当前端行(当前端从条目标记里找,不给调用方添字段)。
    push(std::string(tr("provider_panel.title")));
    std::size_t current_index = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < view.entries.size(); ++i) {
        if (view.entries[i].is_current) {
            current_index = i;
            break;
        }
    }
    push(current_index == static_cast<std::size_t>(-1)
             ? std::string(tr("provider_panel.current_none"))
             : trf("provider_panel.current_line", view.entries[current_index].name));

    push("");
    if (view.entries.empty()) {
        push(std::string(tr("cmd.provider.empty")));
    } else {
        // 窗口:offset 起capacity 行;visible_capacity=0 视为不限,整表画。
        const std::size_t capacity =
            view.visible_capacity == 0 ? view.entries.size() : view.visible_capacity;
        const std::size_t begin = (std::min)(view.offset, view.entries.size() - 1);
        const std::size_t end = (std::min)(begin + capacity, view.entries.size());
        if (begin > 0) {
            push(trf("provider_panel.more_above", begin));
        }
        for (std::size_t i = begin; i < end; ++i) {
            const ProviderPanelEntry& entry = view.entries[i];
            std::string line = i == view.cursor ? "> " : "  ";
            line += entry.name;
            if (entry.is_current) {
                line += tr("cmd.provider.current");
            }
            if (!entry.auth_note.empty()) {
                line += "  " + entry.auth_note;
            }
            push(std::move(line));
        }
        if (end < view.entries.size()) {
            push(trf("provider_panel.more_below", view.entries.size() - end));
        }
    }
    push("");
    push(std::string(tr("provider_panel.footer")));
    return frame;
}

}  // namespace lubancode::cli
