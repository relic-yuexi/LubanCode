#pragma once

#include "agent/loop.hpp"
#include "hooks/dispatcher.hpp"

namespace lubancore::detail {
lubancode::agent::TurnWiring::ActionPreDecision RunPreAction(
    lubancode::hooks::HookDispatcher& dispatcher, const lubancode::hooks::middleware::DispatchTrigger& trigger);
// Append into a private owned copy only after the complete dispatch and all
// supplement budgets have passed. Raw capture has already committed.
std::expected<lubancode::tools::Tool::Result, std::string> RunPostAction(
    lubancode::hooks::HookDispatcher& dispatcher, const lubancode::hooks::middleware::DispatchTrigger& trigger,
    const lubancode::tools::Tool::Result& original);
} // namespace lubancore::detail
