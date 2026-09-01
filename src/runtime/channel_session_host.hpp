// ChannelSessionHost(多渠道消息接入单阶段 3):Headless 渠道会话的宿主。
//
// 单子 §15.1/§15.3 的定案落这里:
//   - 不 new TerminalSessionController(它握着输入框/确认框/面板等终端件);
//     渠道会话复用底层件——SessionRuntime(存档账)、Agent(引擎)、
//     AgentLoop::Run(turn 推进)、TurnEventAdapter(事件出水),这些经
//     ChannelTurnEngine 接口注入,host 自己不装配模型栈。
//   - 单飞铁律:同一 session 同时只跑一轮;同一 conversation 由路由保证
//     同一 session_key,per-session FIFO 即保序;不同 session 可并行,受
//     全局 max_active_channel_turns 限额(§15.3)。
//   - 排队即 WorkKind::ChannelTurn:与用户排队消息同档优先级(§15.3),
//     WorkPriority 的账在 session_work_scheduler,这里只管取件次序。
//
// 泵模型与 ChannelManager 同款(同步泵):Submit() 只入账,PumpOne() 跑
// 一枚。并行怎么来:宿主(未来的 Gateway)开 N 只泵线程各调 PumpOne,
// TryBeginTurn 的门保证单飞与限额在任何线程数下都成立;测试里直接调
// TryBeginTurn 钉合同。
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "agent/loop.hpp"
#include "channel/channel_router.hpp"
#include "runtime/turn_ingress.hpp"

namespace lubancode::runtime {

// 一只 headless 渠道会话的引擎口(装配层实现:AgentChannelEngine 是本仓
// 的真装配,测试/gateway 可换自己的)。一只引擎 = 一场 session(一份
// history、一个存档);同 session_key 的多轮复用同一只。
class ChannelTurnEngine {
public:
    virtual ~ChannelTurnEngine() = default;
    // 跑一轮。同步;reply_text 回填本轮 assistant 正文(阶段 4 的
    // ReplyAssembler 接手前,host 先把粗账带回去),error 回填失败摘要。
    virtual agent::RunOutcome RunTurn(const TurnIngress& ingress, std::string* reply_text,
                                      std::string* error) = 0;
};

// 无远端审批时的工具确认裁定(§16.1 fail closed):
// 工具在 binding allowlist 明确允许且不在 deny -> 允许;其余一律拒绝。
// 这不是"把 confirm 偷换成 auto"——allow 的每一层(binding/账号)都是
// 用户显式写的,没有的默认拒。
bool ChannelConfirmAllows(const channel::ToolRoutePolicy& tools, const std::string& tool_name);

// 渠道侧工具被拒时给模型的 tool_result 文案(区别于"用户拒绝":
// 渠道会话没人守键盘,拒的是"没有审批渠道",on_tool_denial_text 用)。
std::string ChannelToolDenialText(const std::string& tool_name);

class ChannelSessionHost {
public:
    struct Options {
        // 全局并行限额(§15.3 max_active_channel_turns):不同 session 同时
        // 在跑的 turn 数上限。0 = 不限(不推荐,只是别把人锁死)。
        std::size_t max_active_channel_turns = 4;
    };

    explicit ChannelSessionHost(Options options);
    // 缺省装配(限额 4)。不给默认实参是 C++ 的规矩:Options 带 NSDMI,
    // 类内默认实参里用它是不完整类型(禁例清单"NSDMI 默认实参")。
    ChannelSessionHost() : ChannelSessionHost(Options{}) {}
    ~ChannelSessionHost() = default;

    ChannelSessionHost(const ChannelSessionHost&) = delete;
    ChannelSessionHost& operator=(const ChannelSessionHost&) = delete;

    // 引擎工厂:按 session_key 建一只引擎。host 缓存——同一 session_key
    // 只建一次,后续轮次复用同一场 history(resume 语义由缓存承担;跨
    // 进程重启的续接归阶段 9 Gateway 的 session registry)。
    using EngineFactory = std::function<std::unique_ptr<ChannelTurnEngine>(const std::string& session_key)>;
    void SetEngineFactory(EngineFactory factory);

    // 一轮渠道 turn 的收场账。
    struct TurnOutcome {
        std::string session_key;
        std::string ingress_delivery_id;
        bool ok = false;        // RunOutcome 无错且 value 有值
        bool cancelled = false;
        std::string reply_text;  // 本轮 assistant 正文粗账
        std::string error;
    };

    struct SubmitResult {
        enum class Status {
            Queued,
            NoFactory,   // 没装引擎工厂:入不了账,调用方别等
        };
        Status status = Status::Queued;
    };

    // 入账(per-session FIFO;不跑,等泵)。
    SubmitResult Submit(TurnIngress ingress);

    // 跑一枚:按提交序取第一条"session 空闲且未超限额"的待办,调引擎跑
    // 完收场。没有可跑的给 nullopt(队列空或全忙/超限)。
    std::optional<TurnOutcome> PumpOne();

    // ---- 单飞与限额的门(多线程泵/测试直接钉合同用) --------------------
    // 尝试占住一个 turn 槽:session 已在跑 -> single_flight 拒;活跃数达
    // 限额 -> capacity 拒。成功后必须 EndTurn 收闸。
    enum class BeginRefusal { None, SingleFlight, Capacity };
    BeginRefusal TryBeginTurn(const std::string& session_key);
    void EndTurn(const std::string& session_key);

    // ---- 观测 ----
    std::size_t pending_count() const;
    std::size_t pending_count_for(const std::string& session_key) const;
    std::size_t active_turn_count() const;
    bool session_busy(const std::string& session_key) const;
    std::size_t engine_count() const;  // 建过几场 session

private:
    struct PendingTurn {
        std::uint64_t seq = 0;  // 全局提交序(FIFO 基准)
        TurnIngress ingress;
    };

    Options options_;
    EngineFactory factory_;
    mutable std::mutex mutex_;
    std::vector<PendingTurn> pending_;
    std::uint64_t next_seq_ = 1;
    std::size_t active_turns_ = 0;
    std::map<std::string, bool> busy_;                      // session_key -> running
    std::map<std::string, std::unique_ptr<ChannelTurnEngine>> engines_;
};

}  // namespace lubancode::runtime
