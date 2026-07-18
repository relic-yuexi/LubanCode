// Terminal LaTeX math rendering. Unknown commands return empty so callers can
// retain the original delimiter-wrapped source.

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lubancode::cli {

std::optional<std::string> RenderLatexInline(const std::string &latex);
std::optional<std::vector<std::string>> RenderLatexBlock(const std::string &latex);

} // namespace lubancode::cli
