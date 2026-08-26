// Agent 的实现(骨架拆解批四:从 loop.cpp 自立门户)。构造正门只此一只
//(profile);轮次推进在 AgentLoop(loop.cpp),上下文账在 ContextManager。

#include "agent/agent.hpp"

namespace lubancode::agent {

Agent::Agent(api::Backend& backend, tools::ToolRegistry& registry, AgentProfile profile)
    : backend_(backend),
      registry_(registry),
      profile_(std::move(profile)),
      system_prompt_(profile_.system_prompt) {}

std::vector<api::ToolDefinition> Agent::BuildToolDefinitions() const {
    std::vector<api::ToolDefinition> defs;
    defs.reserve(registry_.All().size());
    for (const auto& tool : registry_.All()) {
        // tool_search(延迟挂载):谓词不放行的工具(延迟且未挂载)不进
        // tools 数组。没设谓词就是全量,跟从前一样。每轮现拼而不是构造时
        // 定死,是因为 tool_search 命中会在一次 Run() 中途改变 loaded 集合,
        // 下一轮请求就得看到新挂载的工具。
        if (profile_.tool_filter && !profile_.tool_filter(*tool)) {
            continue;
        }
        defs.push_back(api::ToolDefinition{tool->name(), tool->description(), tool->input_schema()});
    }
    return defs;
}

std::expected<RunOutcome, std::string> Agent::Run(const std::string& user_input, const TurnWiring& wiring,
                                                  const std::atomic<bool>* cancel) {
    api::Message user_message;
    user_message.role = api::Role::User;
    user_message.content.push_back(api::TextBlock{user_input});
    return AgentLoop::Run(*this, std::move(user_message), wiring, cancel);
}

std::expected<RunOutcome, std::string> Agent::Run(api::Message user_message, const TurnWiring& wiring,
                                                  const std::atomic<bool>* cancel) {
    return AgentLoop::Run(*this, std::move(user_message), wiring, cancel);
}

}  // namespace lubancode::agent
