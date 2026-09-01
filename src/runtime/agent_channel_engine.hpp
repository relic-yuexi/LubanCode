// AgentChannelEngine(多渠道消息接入单阶段 3):ChannelTurnEngine 的真装配。
//
// 单子 §15.1 的"复用底层"在这里兑现:一只引擎 = 一场 headless 渠道会话,
// 内持 SessionRuntime(存档/thread 身份/事件接线)与 Agent(引擎本体),
// 每轮走 AgentLoop::Run——与终端会话同一颗 turn engine,不另写 Agent
// loop。与 TerminalSessionController 的分工:那边握终端件(输入框/确认框
// /面板),这边只有无头会话的最小接线:
//   - TurnEventAdapter 照接(事件流出水口;阶段 4 的 ChannelReplySink 挂
//     同一只 EventSink,本批可空)。
//   - 工具确认 fail closed(§16.1):binding allowlist 明确允许才放行,
//     其余拒绝并回稳定文案;没有远端审批渠道,不为"机器人好用"把
//     confirm 偷换成 auto。
//   - memory 隔离(§14.5):allow_memory_retrieval=false 的轮不召回
//     项目/用户记忆(host 侧就不调检索;本类不持 ProjectMemory,装配层
//     把开关折进 BuildTurnContext 的调用与否,这里只透传开关)。
//   - provenance 落档:渠道 user 消息带宿主真账进 session JSONL。
//
// 工具面收窄:ToolRoutePolicy(binding 的 allow/deny)折成 AgentProfile 的
// tool_filter——binding 没命中时 policy 为空(不另设上限,仍受 Agent 自身
// 工具表管;§16.2 的多层交集里,本批落 binding 这一层)。
#pragma once

#include <memory>
#include <string>

#include "agent/agent.hpp"
#include "channel/channel_router.hpp"
#include "runtime/channel_session_host.hpp"
#include "runtime/session_runtime.hpp"
#include "runtime/turn_ingress.hpp"

namespace lubancode::runtime {

class AgentChannelEngine : public ChannelTurnEngine {
public:
    struct Options {
        std::string sessions_dir;   // 旧 SessionStore 档案目录(P0-2 起不消费,P0-6 删)
        std::string workspaces_dir;  // P0-2:唯一持久化根(空 = <home>/.lubancode/workspaces)
        std::string wire_name;      // meta.wire
        std::string model;          // 会话模型(存档 meta 用)
        std::string cwd;            // 会话目录(身份按它四级裁决,不认进程 cwd)
        std::string lubancode_version;
        channel::ToolRoutePolicy tools;  // binding 工具策略(fail closed 裁定用)
    };

    // backend/registry 借用(须活过引擎生命周期);profile 按值收。
    AgentChannelEngine(api::Backend& backend, tools::ToolRegistry& registry,
                       agent::AgentProfile profile, Options options);
    ~AgentChannelEngine() override = default;

    agent::RunOutcome RunTurn(const TurnIngress& ingress, std::string* reply_text,
                              std::string* error) override;

    SessionRuntime& session() { return session_runtime_; }
    agent::Agent& agent() { return agent_; }
    const channel::ToolRoutePolicy& tools() const { return options_.tools; }

private:
    Options options_;
    SessionRuntime session_runtime_;
    agent::Agent agent_;
};

}  // namespace lubancode::runtime
