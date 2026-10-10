#pragma once

#include <ostream>
#include <string>

#include "app/tool_assembly_plan.hpp"
#include "cli/theme.hpp"

namespace lubancode::app {

// CLI policy boundary: discover roots and late-bound CLI state explicitly, then
// hand values/suppliers to the same complete tool assembly used by both modes.
ToolAssemblyPlan ResolveCliToolAssemblyPlan(const std::string& cwd_utf8);

// The returned sink owns its Theme copy and borrows the output stream only for
// the synchronous constructor call. No output destination enters ToolRuntime.
ToolAssemblyDiagnosticSink MakeCliToolAssemblyDiagnosticSink(cli::Theme theme, std::ostream& output);

}  // namespace lubancode::app
