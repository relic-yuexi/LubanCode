#include "agent/task_spec.hpp"

#include "hooks/hash.hpp"

namespace lubancode::agent {

namespace {

bool HasNul(const std::string& text) {
    return text.find('\0') != std::string::npos;
}

bool IsBlank(const std::string& text) {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

}  // namespace

AgentTaskSpec MakeAgentTaskSpec(const std::string& title, const std::string& prompt) {
    AgentTaskSpec spec;
    spec.title = title;
    spec.instructions = prompt;
    return spec;
}

std::string ValidateAgentTaskSpec(const AgentTaskSpec& spec) {
    if (spec.title.empty()) {
        return "title 必填";
    }
    if (IsBlank(spec.instructions)) {
        return "prompt 必填(非空字符串)";
    }
    if (HasNul(spec.instructions)) {
        return "prompt 不允许包含 NUL 字符";
    }
    if (spec.instructions.size() > kMaxTaskSpecBytes) {
        return "prompt 超长(上限 " + std::to_string(kMaxTaskSpecBytes) + " 字节)";
    }
    return {};
}

nlohmann::json CanonicalSpecJson(const AgentTaskSpec& spec) {
    return nlohmann::json{{"schema_version", spec.schema_version},
                          {"title", spec.title},
                          {"instructions", spec.instructions}};
}

std::string TaskSpecHash(const AgentTaskSpec& spec) {
    const std::string full = lubancode::hooks::Sha256Hex(CanonicalSpecJson(spec).dump());
    return full.size() > 16 ? full.substr(0, 16) : full;
}

}  // namespace lubancode::agent
