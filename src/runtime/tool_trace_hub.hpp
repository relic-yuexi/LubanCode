// ToolTraceHub(逐枚追踪单"一份事件,两路消费"):canonical 领域事件的
// 装配层分线器。
//
// AgentLoop 只吐一份 ToolTraceEvent(经 Callbacks::on_tool_trace);本类
// 把它分发到三处:
//   - Runtime EventSink(TurnEventAdapter 的 ServerEvent 投影,UI 侧)
//   - SessionTraceSink(session JSONL 的 durable 栅栏,append+flush)
//   - Workflow projection(录制开启时的派生账,只吃脱敏摘要)
//
// 规矩(单子原文):
//   - 领域事件不带 ANSI,不预先翻中文;seq 由 Runtime 唯一发号。
//   - sink 各自失败,各自报稳定错误;UI 失败不拦工具。
//   - durable started 写失败按 effect class 决定是否拦执行:副作用未知/
//     不可逆/远端档拦(返回 false,RunOneTool 不 execute);只读本地档
//     放行但当场告警(EventSink 的 Warning)。
//   - 终态唯一。迟到、重复终态不覆盖原账,只记 protocol violation。
//
// 依赖:agent(ToolTraceEvent/SessionStore)+ runtime(EventSink/发号),
// 不认 cli/app。线程安全:落盘口一把 mutex(一场 session 一只 writer,
// 单线程落盘,并发投递在这里排成队——单子"并发写")。

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "agent/loop.hpp"
#include "agent/session_store.hpp"
#include "agent/tool_trace.hpp"
#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/id_authority.hpp"

namespace lubancode::runtime {

// 会话侧的工具追踪分线器。一只 SessionRuntime 配一只,跨轮存活。
class ToolTraceHub {
public:
    // 持久化口径:process-crash(append+flush)是默认;power-loss
    // (fdatasync/FlushFileBuffers)另立开关,不默认开(单子 Durability 节
    // ——两档都要明说,不许吹)。
    struct Options {
        bool power_loss_durable = false;  // started/finished 额外 fsync;增延迟,不默认
    };

    ToolTraceHub(IdAuthority& ids, agent::SessionStore* store);
    ToolTraceHub(IdAuthority& ids, agent::SessionStore* store, const Options& options);
    ~ToolTraceHub();

    ToolTraceHub(const ToolTraceHub&) = delete;
    ToolTraceHub& operator=(const ToolTraceHub&) = delete;

    // Runtime 事件落点(可空:终端老路只落 session 不上事件流)。装了
    // app-server/Json sink 的会话在这里接上,工具相位随 ServerEvent 走。
    void AttachSink(EventSink* sink) { sink_ = sink; }

    // Workflow projection(可空):录制开启时由装配层挂上,只吃 execution_
    // id/outcome/error_code/摘要——不吃原始入参/结果正文(单子"WorkflowRecorder
    // 只作派生账")。
    using Projection = std::function<void(const agent::ToolTraceEvent&)>;
    void AttachProjection(Projection projection) { projection_ = std::move(projection); }

    // AgentLoop 的 execution 发号口(IdAuthority 的 item id 同源;单子:
    // 不自造第二只计数器)。
    std::string NextExecutionId();

    // 装配 AgentLoop 的三个 trace 关口。调用方把返回的 lambdas 塞进
    // Callbacks,loop 自己不知道 hub 存在(依赖单向)。
    //   on_trace      —— canonical 事件分线(栅栏持久化 + UI 投影 + 录制)
    //   on_assistant  —— assistant 消息 append+flush(批次头,单子落盘次序 1-2)
    //   on_results    —— tool result 消息 append+flush + 各枚 result_committed
    //                    (单子落盘次序 6-7)
    // 返回 false 的 started(副作用工具写不落)会在 OnTrace 里直接拦:
    // 拦的方式是抛出 kErrSessionTraceAppendFailed 的拦执行信号——见
    // OnTrace 的注释,AgentLoop 侧由 RunOneTool 在 execute 前查询。
    void Install(agent::Agent& loop, agent::Callbacks& callbacks, const std::string& thread_id,
                 const std::string& turn_id);

    // 单笔分发(Install 之外的手工投递口:恢复侧、测试、后台子代理的
    // 只读 sink 并轨)。
    void OnTrace(const agent::ToolTraceEvent& event);

    // 恢复:从 LoadedSession 的 trace 事件折叠账本(/resume 侧调)。
    static agent::ToolExecutionLedger BuildLedger(const std::vector<agent::ToolTraceEvent>& events);

    // 诊断:最近一批的摘要(/trace 用)。返回空串 = 没账。
    std::string LastBatchSummary() const;
    // 诊断:本 session 全部明确失败与 unknown(/trace errors)。
    std::vector<std::string> ErrorLines() const;

    // 条件式撤销的查表口(undo_file_edit 用):按 execution_id 翻账本里
    // 那枚执行留下的 undo token。进程内 recent_ 账优先,查不到回落折叠
    // 存档真本(重启后 /trace 仍可查,撤销也仍可用)。nullopt = 没有。
    std::optional<agent::ToolUndoToken> FindUndoToken(const std::string& execution_id) const;
    // 一枚 undo 补偿的目标(token 的主人):recent_ 里 execution_id 对应
    // 的记录若自己带 parent/retry 关系就顺着报,否则原样返回 execution_id
    //(undo_file_edit 的 compensates 语义由调用方拼,这里只给账)。
    std::string OwnerOfExecution(const std::string& execution_id) const;

    // 当前进行中的 agent 工具调用的 execution_id(子代理事件的 parent
    // 关系边)。主会话 RunOneTool 执行 "agent" 工具期间由装配层推进;
    // 没在跑 agent 工具时为空——子代理事件照发,parent 如实缺边。
    std::string current_agent_execution() const;
    void set_current_agent_execution(std::string execution_id);

    // 某一轮 turn 的 finished 执行快照(goal 采证用):按投递序拷出
    // recent_ 里 turn_id 匹配、kind=ExecutionFinished 的事件。进程内账
    // 有界(512 枚),老轮次走 BuildLedger 折叠存档——采证发生在轮收口
    // 当拍,recent_ 必然覆盖。轮 id 为空给空表。
    std::vector<agent::ToolTraceEvent> FinishedEventsOfTurn(const std::string& turn_id) const;

private:
    void EmitRuntimeEvent(const agent::ToolTraceEvent& event);
    // started 栅栏是否拦执行:副作用未知/不可逆档写不落即拦;只读档放行
    // 并告警(单子"execution_started 在 Tool::execute 前写并 flush。写不成
    // 时,副作用工具不得继续执行;只读工具可按明确降级策略执行,但须当场
    // 告警")。
    bool ShouldBlockOnFailedStart(agent::EffectClass cls) const;

    IdAuthority& ids_;
    agent::SessionStore* store_;  // 不持有;空 = 只投影不落盘
    Options options_;
    EventSink* sink_ = nullptr;
    Projection projection_;

    ServerEvent MakeWarningEvent(const agent::ToolTraceEvent& event, const std::string& code);

    mutable std::mutex mutex_;
    std::vector<agent::ToolTraceEvent> recent_;     // 进程内最近账(诊断用)
    std::string last_batch_id_;
    std::string thread_id_;
    std::string turn_id_;
    std::set<std::string> blocked_executions_;      // started 落盘失败被拦的 execution
    std::string current_agent_execution_;           // 进行中的 agent 调用(parent 边)
    std::uint64_t dropped_count_ = 0;               // 非关键事件丢弃计数(队列满口径)
};

}  // namespace lubancode::runtime
