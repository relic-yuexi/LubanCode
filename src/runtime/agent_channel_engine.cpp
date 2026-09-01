// AgentChannelEngine 实现(多渠道消息接入单阶段 3)。装配合同见头文件。
#include "runtime/agent_channel_engine.hpp"

namespace lubancode::runtime {

namespace {

// §16.2 的权限交集在暴露面执法:binding 的 allow/deny 叠进 AgentProfile 的
// tool_filter(原 profile 已有过滤的先过,再过渠道层——每层只收窄)。
// 被滤掉的工具模型看都看不见;看得见但 needs_confirm 的调用点再由
// on_tool_confirm 的 fail closed 裁定(两层各管一段)。
agent::AgentProfile ApplyChannelToolPolicy(agent::AgentProfile profile,
                                           const channel::ToolRoutePolicy& policy) {
    if (policy.allow.empty() && policy.deny.empty()) {
        return profile;  // binding 没设上限:不添乱,交给 Agent 自身工具表
    }
    auto prior = profile.tool_filter;
    const channel::ToolRoutePolicy policy_copy = policy;
    profile.tool_filter = [prior, policy_copy](const tools::Tool& tool) {
        if (prior && !prior(tool)) {
            return false;
        }
        return policy_copy.Allows(tool.name());
    };
    profile.tool_filter_denial =
        "channel.binding_denied|该工具不在渠道 binding 的 tools 允许名单内"
        "(allowlist 没列或进了 deny)。要放行须在全局 config 的渠道 binding 显式声明。";
    return profile;
}

}  // namespace

AgentChannelEngine::AgentChannelEngine(api::Backend& backend, tools::ToolRegistry& registry,
                                       agent::AgentProfile profile, Options options)
    : options_(std::move(options)), session_runtime_(SessionRuntime::Options{
                                        .sessions_dir = options_.sessions_dir,
                                        .wire_name = options_.wire_name,
                                        .start_ts = sessions::NowIdTimestamp(),
                                        .lubancode_version = options_.lubancode_version,
                                    }),
      agent_(backend, registry, ApplyChannelToolPolicy(std::move(profile), options_.tools)) {}

agent::RunOutcome AgentChannelEngine::RunTurn(const TurnIngress& ingress, std::string* reply_text,
                                              std::string* error) {
    if (reply_text != nullptr) reply_text->clear();
    if (error != nullptr) error->clear();

    // 首条文本做建档 slug(与终端路同款)。
    std::string first_text;
    for (const auto& block : ingress.message.content) {
        if (const auto* tb = std::get_if<api::TextBlock>(&block)) {
            first_text = tb->text;
            break;
        }
    }
    session_runtime_.EnsureBegun(first_text, options_.model, options_.cwd);

    const std::size_t history_before = agent_.history().size();

    // 事件出水:每轮一只适配器(与终端路同款;sink 没挂就只发号)。
    TurnEventAdapter turn_events = session_runtime_.MakeTurnAdapter();

    // 渠道轮的最小接线:无终端、无远端审批——工具确认 fail closed。
    agent::TurnWiring wiring;
    wiring.events = &turn_events;
    const channel::ToolRoutePolicy& tools = options_.tools;
    wiring.on_tool_confirm = [&tools](const std::string& /*tool_use_id*/,
                                      const std::string& name, const nlohmann::json& /*input*/) {
        return ChannelConfirmAllows(tools, name);
    };
    wiring.on_tool_denial_text = [](const std::string& /*tool_use_id*/,
                                    const std::string& name) {
        return ChannelToolDenialText(name);
    };

    const auto outcome = agent_.Run(ingress.message, wiring);
    if (!outcome.has_value()) {
        if (error != nullptr) {
            *error = outcome.error();
        }
    }

    // 回复正文粗账:本轮新增 assistant 消息的 TextBlock 拼接(空正文不
    // 造块——阶段 4 ReplyAssembler 只吃 ServerEvent,这份只是 host 的
    // 回执底账)。
    if (reply_text != nullptr) {
        const auto& history = agent_.history();
        for (std::size_t i = history_before; i < history.size(); ++i) {
            if (history[i].role != api::Role::Assistant) continue;
            for (const auto& block : history[i].content) {
                if (const auto* tb = std::get_if<api::TextBlock>(&block)) {
                    *reply_text += tb->text;
                }
            }
        }
    }

    // 落档:渠道 user 消息带 provenance(message-contracts.md §2)。
    session_runtime_.PersistNewWithProvenance(agent_.history(), options_.model, options_.cwd,
                                              ingress.provenance);
    return outcome.has_value() ? *outcome : agent::RunOutcome{};
}

}  // namespace lubancode::runtime
