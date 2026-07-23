#include "cli/choice_menu.hpp"

#include <algorithm>

namespace lubancode::cli {

ChoiceMenuCore::ChoiceMenuCore(std::size_t item_count, bool multi_select)
    : multi_select_(multi_select) {
    state_.selected.assign(item_count, false);
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
            if (multi_select_ && event.ch == U' ') {
                state_.selected[state_.cursor] = !state_.selected[state_.cursor];
            }
            break;
        case KeyKind::Enter:
        case KeyKind::NewLine:
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
