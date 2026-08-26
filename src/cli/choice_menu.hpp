#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "cli/line_editor.hpp"

namespace lubancode::cli {

struct ChoiceMenuState {
    std::size_t cursor = 0;
    std::vector<bool> selected;
    bool submitted = false;
    bool cancelled = false;
    bool invalid = false;
    bool custom_submitted = false;
    std::string custom_text;
};

class ChoiceMenuCore {
public:
    ChoiceMenuCore(std::size_t item_count, bool multi_select,
                   std::optional<std::size_t> editable_index = std::nullopt,
                   std::size_t initial_cursor = 0,
                   std::optional<std::size_t> immediate_submit_index = std::nullopt);

    const ChoiceMenuState& state() const { return state_; }
    const ChoiceMenuState& HandleKey(const KeyEvent& event);
    std::vector<std::size_t> SelectedIndices() const;
    std::optional<std::size_t> editable_index() const { return editable_index_; }

private:
    bool multi_select_ = false;
    std::optional<std::size_t> editable_index_;
    std::optional<std::size_t> immediate_submit_index_;
    ChoiceMenuState state_;
};

}  // namespace lubancode::cli
