#pragma once

#include "lubancore/core.hpp"
#include "api/backend.hpp"
#include "tools/tool.hpp"

namespace lubancore::detail {
// The shared control block guards final public Backend destruction on every thread.
std::shared_ptr<Backend> OwnBackend(std::unique_ptr<Backend>&);
std::unique_ptr<lubancode::api::Backend> AdaptBackend(std::shared_ptr<Backend>);
std::unique_ptr<lubancode::tools::Tool> BindLocalTool(
    std::unique_ptr<lubancode::tools::Tool>, std::string cwd, bool command_jobs = false);
Result<std::unique_ptr<lubancode::tools::Tool>> AdaptTool(Tool, std::string cwd);
} // namespace lubancore::detail
