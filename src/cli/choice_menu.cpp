#include "cli/choice_menu.hpp"

#include <algorithm>

namespace lubancode::cli {

namespace {

void AppendUtf8(std::string& out, char32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void EraseLastUtf8(std::string& text) {
    if (text.empty()) {
        return;
    }
    std::size_t pos = text.size() - 1;
    while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) {
        --pos;
    }
    text.erase(pos);
}

bool HasVisibleText(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char ch) { return ch > 0x20 && ch != 0x7F; });
}

void AppendPastedText(std::string& out, const std::string& pasted) {
    for (const char ch : pasted) {
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            out.push_back(' ');
        } else if (ch != '\0') {
            out.push_back(ch);
        }
    }
}

}  // namespace

ChoiceMenuCore::ChoiceMenuCore(std::size_t item_count, bool multi_select,
                               std::optional<std::size_t> editable_index,
                               std::size_t initial_cursor)
    : multi_select_(multi_select), editable_index_(editable_index) {
    state_.selected.assign(item_count, false);
    if (editable_index_.has_value() && *editable_index_ >= item_count) {
        editable_index_.reset();
    }
    // 初始高亮:超出范围就钳到首项,免得越界。
    state_.cursor = (item_count > 0 && initial_cursor < item_count) ? initial_cursor : 0;
}

const ChoiceMenuState& ChoiceMenuCore::HandleKey(const KeyEvent& event) {
    if (state_.submitted || state_.cancelled || state_.selected.empty()) {
        return state_;
    }
    state_.invalid = false;
    switch (event.kind) {
        case KeyKind::Up:
        case KeyKind::ShiftTab:
            state_.cursor = state_.cursor == 0 ? state_.selected.size() - 1 : state_.cursor - 1;
            break;
        case KeyKind::Down:
        case KeyKind::Tab:
            state_.cursor = (state_.cursor + 1) % state_.selected.size();
            break;
        case KeyKind::Home:
            state_.cursor = 0;
            break;
        case KeyKind::End:
            state_.cursor = state_.selected.size() - 1;
            break;
        case KeyKind::Char:
            if (multi_select_ && event.ch == U' ' && state_.cursor != editable_index_) {
                state_.selected[state_.cursor] = !state_.selected[state_.cursor];
            } else if (editable_index_.has_value() && event.ch >= 0x20 && event.ch != 0x7F) {
                state_.cursor = *editable_index_;
                AppendUtf8(state_.custom_text, event.ch);
            }
            break;
        case KeyKind::Paste:
            if (editable_index_.has_value()) {
                state_.cursor = *editable_index_;
                AppendPastedText(state_.custom_text, event.text);
            }
            break;
        case KeyKind::Backspace:
            if (editable_index_.has_value() && state_.cursor == *editable_index_) {
                EraseLastUtf8(state_.custom_text);
            }
            break;
        case KeyKind::Enter:
        case KeyKind::NewLine:
            if (editable_index_.has_value() && state_.cursor == *editable_index_) {
                state_.custom_submitted = HasVisibleText(state_.custom_text);
                state_.submitted = state_.custom_submitted;
                state_.invalid = !state_.submitted;
                break;
            }
            if (!multi_select_) {
                state_.selected[state_.cursor] = true;
                state_.submitted = true;
                break;
            }
            state_.submitted =
                std::find(state_.selected.begin(), state_.selected.end(), true) != state_.selected.end();
            state_.invalid = !state_.submitted;
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

std::vector<std::size_t> ChoiceMenuCore::SelectedIndices() const {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < state_.selected.size(); ++i) {
        if (state_.selected[i]) {
            out.push_back(i);
        }
    }
    return out;
}

}  // namespace lubancode::cli
