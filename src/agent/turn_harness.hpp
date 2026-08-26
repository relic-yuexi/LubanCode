// TurnHarness(骨架拆解批三:harness 合流)。主回合(turn_runner 的 RunTurn)
// 与子代理(agent_tool 的 RunTask)从前各配一副鞍辔:Stop 钩子续跑环同一
// 模式两份代码、取消链各养各的、收场分型一个 footer tone 一个 TaskOutcome。
// 这只 harness 把三样各收成一份,住 engine 层(agent/),两边的装配层只管
// 换皮——渲染三档(终端画/静默查看态/事件流)经 Callbacks 装配表达,harness
// 本身零终端依赖:
//   - CancelChain:取消链。单信号直通(主回合监听线程那根,行为与从前一字
//     不差);多信号(子代理:面板 x + 父轮 ESC + 墙钟看门狗)起一只 20ms
//     粒度合并线程。
//   - ClassifyTurnEnd:收场分型。RunOutcome 的原始信号 + 终局上下文 ->
//     status/reason 一份;主回合拿它映射 footer tone,子代理拿它填
//     TaskOutcome,文案各归各(分型只此一份,措辞是展示)。
//   - DriveTurn:续投外环。Run -> 分型短路 -> 续投源问询(子代理的 inbox
//     原子交接;主回合没有,单轮即收)。从前两边的 for 循环体都从这只过。
//   - RunStopContinuation:Stop/SubagentStop 钩子续跑环。单列一只函数而不
//     并进 DriveTurn,因为两家的调用时机不同——主回合在终端 chrome 收妥
//     (监听线程停、footer 拆)之后跑,子代理在合并线程 join 之后跑;续跑
//     轮拿到的 cancel 语义靠这个次序保真(子代理的合并旗 join 时已置真,
//     续跑轮随之立即收场,与合流前一致)。
//
// hooks 的发射口(主回合 dispatcher->Emit / 子代理前台 EmitWith / 后台
// DetachedHookSession::Emit)经 std::function 递进来,harness 不认 dispatcher
// 的具体形状——它只认"发一枚 stop 事件,拿回归并结果"。
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agent/loop.hpp"
#include "hooks/types.hpp"

namespace lubancode::agent {

// ---------------------------------------------------------------------------
// 取消链:把 N 根外部停止信号并成一根给 AgentLoop::Run。
// ---------------------------------------------------------------------------
class CancelChain {
public:
    CancelChain() = default;
    ~CancelChain();

    CancelChain(const CancelChain&) = delete;
    CancelChain& operator=(const CancelChain&) = delete;

    // 加一根信号(Start 之前加齐)。空指针忽略——"没有这路信号"是常态
    //(主回合没有面板 x,后台任务没有父轮 ESC)。
    void Add(const std::atomic<bool>* signal);

    // 合并后的信号地址。只加了一根时直通原地址(不起线程,与从前单信号
    // 透传一字不差);零根返回 nullptr;多根起合并线程轮询,任何一根置位
    // 即合并置位。重复调用返回同一地址。
    const std::atomic<bool>* Start();

    // 收链:置真合并旗、唤醒合并线程并 join。Run() 返回后调用方务必在跑
    // Stop 续跑环之前调(合流前的次序);不调析构也会兜底。
    void Stop();

private:
    std::vector<const std::atomic<bool>*> signals_;
    std::unique_ptr<std::atomic<bool>> merged_ptr_;  // 多信号时的合并旗
    std::optional<std::thread> merger_;
    const std::atomic<bool>* result_ = nullptr;
};

// ---------------------------------------------------------------------------
// 收场分型(一份):主回合 footer tone 与子代理 TaskOutcome 的共同源头。
// ---------------------------------------------------------------------------

// 分型结论。status 是四态收口;reason 是短因(子代理面板/通知用,主回合
// 只认 status)。
struct TurnVerdict {
    enum class Status { Completed, Stopped, BudgetExhausted, Failed };
    enum class Reason {
        None,                 // 正常完成
        UserStop,             // 用户中止(ESC/面板 x)
        StepLimit,            // 步数预算用满
        OutputBudget,         // 输出预算耗尽(续跑用完仍无正文)
        ApiError,             // 接口报错(请求失败/流中断)
        MaxContext,           // 上下文装不下
        NoFinalText,          // 最后一轮没有文本结论
        ProtocolError,        // 会话协议异常(连历史都没有)
        WallClockTimeout,     // 整轮墙钟上限兜底
    };

    Status status = Status::Failed;
    Reason reason = Reason::None;

    // 稳定短名(completed/stopped/budget_exhausted/failed),结果文本与
    // 测试对账用。
    static const char* StatusTag(Status status);
};

// 分型输入:harness 交回的原始信号(DriveReport 平铺)+ 终局上下文。
struct TurnEndgame {
    bool cancelled = false;             // 有轮被打断
    bool hit_step_limit = false;        // 有轮撞步数闸
    bool wall_clock = false;            // 墙钟看门狗到点(置位时压过 cancelled)
    std::string error;                  // 有轮报错(Run 的 unexpected 文案)
    bool history_empty = false;         // 连一条 assistant 都没有
    std::string final_text;             // 末条 assistant 的文本结论(可空)
    bool output_budget_exhausted = false;  // length 续跑用完仍无正文
    // 子代理要文本结论(空文本 = NoFinalText 失败);主回合不要(模型只回
    // 工具或空轮也按 Worked 收)。差别写在这儿,不藏在各家的 if 里。
    bool require_final_text = true;
};

// 纯函数,单测钉:各态分型。裁定次序与合流前两家一致——墙钟 > 打断 >
// 步数闸 > 报错 > 空历史 > 输出预算 > 无结论 > 完成。报错文案含"上下文"
// 按 MaxContext 分(沿用旧启发式,不改判据)。
TurnVerdict ClassifyTurnEnd(const TurnEndgame& end);

// ---------------------------------------------------------------------------
// 续投外环(一份):Run -> 短路 -> 续投。
// ---------------------------------------------------------------------------

// 续投批次:一轮正常收口后从续投源领出的一批增量("Queued 是交付承诺")。
// input 是拼好的下一轮 user 输入;restore 在"领了批次的那轮失败/被打断/
// 撞限"时由 harness 调用——取走了不等于送到了,按批退回未送。
struct ContinuationBatch {
    std::string input;
    std::function<void()> restore;
};

// 续投源:一轮正常收口(非打断、非错误、非预算耗尽)后调一次。返回
// nullopt = 封账,可进终态;返回批次 = 再开一轮。cancel 已置位时 harness
// 不会调它(先短路)。
using ContinuationSource = std::function<std::optional<ContinuationBatch>()>;

// DriveTurn 的装配材料。
struct DriveOptions {
    const std::atomic<bool>* cancel = nullptr;       // 取消链出来的那根
    ContinuationSource continuation;                  // 空 = 单轮(主回合)
    // 墙钟:开着才在轮间查(看门狗到点把流掐断,loop 按打断收场——这不是
    // 用户中止)。空函数 = 没开墙钟(主回合/默认)。
    std::function<bool()> wall_clock_fired;
    // 每轮正常收口的观察口(RunOutcome 交账:子代理记步数进台账、封卷
    // 消息账)。空 = 不看。
    std::function<void(const RunOutcome&)> on_round_settled;
};

// DriveTurn 的交账:原始信号,不分型(分型归 ClassifyTurnEnd,两边的
// 终局上下文各归各补)。
struct DriveReport {
    bool ok = true;               // 最后一轮 Run 是否正常交账(false = error 有货)
    bool cancelled = false;       // 任一轮被打断(含续投领批后发现的取消)
    bool hit_step_limit = false;  // 任一轮撞步数闸
    bool wall_clock = false;      // 轮间查到墙钟到点
    std::string error;            // !ok 时的错误文案
    std::string stop_reason;      // 最后一轮正常收口的 stop_reason
    int steps_used = 0;           // 全部轮次(含 Stop 续跑轮)的模型请求数合计
    std::optional<OutputBudgetReport> output_budget;  // 最后一份输出预算账(含续跑轮的)
    // 最后一个正常收口轮的原始 RunOutcome。length_empty_output 这类"轮内
    // 状态"从这取——主回合读它还原 RunOutcome;Stop 续跑轮的 outcome 只并
    // 步数/预算/打断三笔进上面的字段,不覆盖这只(合流前主回合正是这么
    // 读的:续跑轮的 length_empty 不改主轮的账)。
    std::optional<RunOutcome> final_round;
};

// 续投外环(主回合与子代理共用的那份循环本体):
//   for(;;) {
//     outcome = agent.Run(input, callbacks, cancel)
//     报错 -> 退批、记错、收
//     打断/墙钟/撞限 -> 退批、记旗、收
//     领续投批 -> 没批(封账)收;有批再跑一轮
//   }
// 异常不在这里接——主回合的回合级兜底(try/catch)与子代理的线程兜底照旧
// 罩在外层,harness 不抢它们的活。
// input 是首轮的完整 user 消息(可带图像附件);续投轮的输入是字符串,
// 走 Agent::Run 的字符串重载。
DriveReport DriveTurn(Agent& agent, const TurnWiring& wiring, api::Message input,
                      const DriveOptions& options);

// ---------------------------------------------------------------------------
// Stop 续跑环(一份):钩子拉闸(continue=false)且没续过 -> 再收口一轮,
// stop_hook_active 防咬尾,最多续一次;续跑轮报错/打断/撞限就如实停。
// ---------------------------------------------------------------------------

// Stop/SubagentStop 的发射口:递 stop_hook_active 与末条 assistant 文本,
// 拿回归并结果。主回合/子代理前台/后台各包各的 dispatcher,这里不认。
using StopHookEmit =
    std::function<hooks::HookEventResult(bool stop_hook_active, const std::string& last_text)>;

// Stop 续跑环的装配材料。
struct StopOptions {
    StopHookEmit emit;  // 空 = 没配 stop 钩子,整环跳过
    const std::atomic<bool>* cancel = nullptr;  // 续跑轮的取消(次序语义见文件头)
    std::string label;  // 续跑输入前缀:"[stop 钩子续跑,非用户输入] " /
                        // "[SubagentStop 钩子续跑,非用户输入] "
    // 每轮续跑收口的观察口(子代理记步数台账)。
    std::function<void(const RunOutcome&)> on_round;
    // 钩子要求续跑时的报信口(主回合打一行"[stop 钩子] 要求再收口一轮:
    // ...";子代理没有,空函数)。reason 是钩子给的理由。
    std::function<void(const std::string& reason)> on_continue_request;
    // 末条 assistant 文本的取法。主回合冻结在环前(从前的语义:续跑轮
    // 不刷新它);子代理每轮现取。差别写在这儿。
    std::function<std::string()> final_text;
};

// 跑 Stop 续跑环。steps_used/output_budget 的增量并进 drive 交来的账
//(子代理的口径;主回合不读这两个字段)。cancelled 只添不改(打断收场
// 时调用方压根不该调这函数——与合流前两家的门一致)。
void RunStopContinuation(Agent& agent, const TurnWiring& wiring, const StopOptions& options,
                         DriveReport& report);

}  // namespace lubancode::agent
