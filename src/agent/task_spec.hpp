// 子代理活执行合同：标题给人看，instructions 原样交给子代理。
#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::agent {

struct AgentTaskSpec {
    int schema_version = 2;
    std::string title;
    std::string instructions;

    bool operator==(const AgentTaskSpec&) const = default;
};

inline constexpr std::size_t kMaxTaskSpecBytes = 32 * 1024;

AgentTaskSpec MakeAgentTaskSpec(const std::string& title, const std::string& prompt);
std::string ValidateAgentTaskSpec(const AgentTaskSpec& spec);
nlohmann::json CanonicalSpecJson(const AgentTaskSpec& spec);

std::string TaskSpecHash(const AgentTaskSpec& spec);

}  // namespace lubancode::agent
