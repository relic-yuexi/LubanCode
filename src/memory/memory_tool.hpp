#pragma once

#include <memory>

#include "memory/project_memory.hpp"
#include "tools/tool.hpp"

namespace lubancode::memory {

class MemorySaveTool final : public tools::Tool {
public:
    explicit MemorySaveTool(std::shared_ptr<ProjectMemory> memory) : memory_(std::move(memory)) {}

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    Result execute(const nlohmann::json& input) override;

private:
    std::shared_ptr<ProjectMemory> memory_;
};

}  // namespace lubancode::memory
