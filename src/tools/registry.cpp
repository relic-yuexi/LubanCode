#include "tools/registry.hpp"

#include <utility>

namespace lubancode::tools {

void ToolRegistry::Register(std::unique_ptr<Tool> tool) {
    tools_.push_back(std::move(tool));
}

Tool* ToolRegistry::Find(const std::string& name) const {
    for (const auto& tool : tools_) {
        if (tool->name() == name) {
            return tool.get();
        }
    }
    return nullptr;
}

}  // namespace lubancode::tools
