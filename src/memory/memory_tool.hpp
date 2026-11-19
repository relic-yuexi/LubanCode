#pragma once

#include <memory>
#include <functional>
#include <expected>

#include "memory/project_memory.hpp"
#include "tools/tool.hpp"

namespace lubancode::memory {

// Both the queued CLI and synchronous SDK target parse this same tool input.
std::expected<SaveRequest, std::string> ParseMemorySaveInput(const nlohmann::json& input);

class MemorySaveTool final : public tools::Tool {
public:
    explicit MemorySaveTool(std::shared_ptr<ProjectMemory> memory) : memory_(std::move(memory)) {}
    using SaveTarget = std::function<Result(std::expected<SaveRequest, std::string>, const tools::ToolExecutionContext&)>;
    explicit MemorySaveTool(SaveTarget target) : target_(std::move(target)) {}

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    Result execute(const nlohmann::json& input) override;
    Result execute(const nlohmann::json& input, const tools::ToolExecutionContext& context) override;

private:
    std::shared_ptr<ProjectMemory> memory_;
    SaveTarget target_;
};

}  // namespace lubancode::memory
