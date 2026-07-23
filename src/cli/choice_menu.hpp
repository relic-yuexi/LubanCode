#pragma once

#include <cstddef>
#include <vector>

#include "cli/line_editor.hpp"

namespace lubancode::cli {

struct ChoiceMenuState {
    std::size_t cursor = 0;
    std::vector<bool> selected;
    bool submitted = false;
    bool cancelled = false;
    bool invalid = false;
};

class ChoiceMenuCore {
public:
    ChoiceMenuCore(std::size_t item_count, bool multi_select);

    const ChoiceMenuState& state() const { return state_; }
    const ChoiceMenuState& HandleKey(const KeyEvent& event);
    std::vector<std::size_t> SelectedIndices() const;

private:
    bool multi_select_ = false;
    ChoiceMenuState state_;
};

}  // namespace lubancode::cli
