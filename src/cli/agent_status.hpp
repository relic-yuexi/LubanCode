// 子代理状态条(#52):RunTurn 期间在输入区上方常驻的一块状态展示——
// 子代理(agent 工具)执行中显示任务摘要、耗时、已调用工具次数,结束后
// 收成一行结果(成功/失败 + 耗时)。
//
// 引擎现状:agent::AgentLoop::Run() 里工具调用是严格顺序执行的 for 循环
// (RunOneTool),同一时刻最多一个子代理在跑——这块状态条不假装真并发,
// 只是"跑完一个收一行、紧接着下一个又展开一行"地累积成列表,一轮
// (RunTurn)开始时清空、结束时随最后一行一起清场。
//
// 两块:
//   1. AgentStatusBoard —— 线程安全的状态容器(主线程在 ToolDisplay 回调
//      里写,main.cpp 的 ticker 线程周期性读 Snapshot() 刷屏),纯逻辑,
//      不碰 IO,单测主战场。
//   2. FormatAgentStatusLines —— 纯渲染函数,状态条 → 每行文本(不带尾随
//      \n),main.cpp 里的 ticking 画板只管拿这些行去刷屏,不用重新想
//      措辞/格式。
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "cli/theme.hpp"

namespace lubancode::cli {

enum class AgentStatusState { Running, Ok, Error };

struct AgentStatusEntry {
    int id = 0;
    std::string label;  // 任务摘要,BuildToolTitle 同款 40 码点截断规矩
    AgentStatusState state = AgentStatusState::Running;
    int tool_calls = 0;
    // 这个子代理自己发出的请求的 token 用量(输入+输出)累计——跟父级
    // usage_stats 是两本账,父级那本连子代理的量一起算总花费,这本只认
    // "这一个子代理任务花了多少",摘要行 "Xk tokens" 那一节用。
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::chrono::steady_clock::time_point start_time{};
    std::chrono::steady_clock::time_point end_time{};  // state != Running 才有意义
};

// 线程安全:主线程(ToolDisplay 回调)写,ticker 线程(main.cpp 里的画板
// 定时唤醒)读 Snapshot() 刷屏,内部一把 mutex 全程护着。
class AgentStatusBoard {
public:
    // 新起一个子代理条目,Running 态,start_time 记现在,返回分配的 id
    // (RecordToolCall/Finish 认这个 id)。label 里的换行/制表符压成空格、
    // 按 40 码点截断——跟 transcript.cpp::BuildToolTitle 对 "agent" 工具
    // 参数摘要的处理逐字一致,两处显示的任务摘要看着是同一个东西。
    int Start(std::string label);

    // 子代理内部调了一次工具,对应条目的 tool_calls 计数 +1;id 认不出
    // 或者条目已经收尾(Clear() 之后晚到的回调、或 Finish 之后的孤儿事件)
    // 时安静地什么也不做。
    void RecordToolCall(int id);

    // 子代理这一次独立请求的 token 用量累进对应条目——每次 on_usage 触发
    // 都调一次,累计口径同 tool_calls(id 认不出/条目已收尾安静地什么也
    // 不做)。摘要行的 "Xk tokens" 那一节数据来源。
    void RecordUsage(int id, std::int64_t input_tokens, std::int64_t output_tokens);

    // 子代理跑完,收成终态(success 决定 Ok/Error),end_time 记现在;id
    // 认不出或已经收尾过同样安静地什么也不做。
    void Finish(int id, bool success);

    // 清空整块(每轮 RunTurn 开始前调一次,别让上一轮的收尾行拖进下一轮)。
    void Clear();

    std::vector<AgentStatusEntry> Snapshot() const;

    // 至少有一条 Running 态——ticker 线程拿这个判断"还要不要继续跳"。
    bool HasRunning() const;

    bool Empty() const;

private:
    mutable std::mutex mutex_;
    std::vector<AgentStatusEntry> entries_;
    int next_id_ = 1;
};

// 纯渲染:把 entries 排成要打印的那几行(不带尾随 \n)。骨架照 Claude Code
// 的子代理摘要块靠(三行一组):
//   ● <任务摘要>
//     ⎿  <状态词>(<N> 次工具调用 · <X>k tokens · <耗时>s)
//     (ctrl+o 展开明细)
// 每条 entry 固定占 3 行(entries.size() * 3),第三行是折叠提示,跑动中/
// 收尾态都带着——Ctrl+O 随时能展开,不必等跑完才提示。now 只用于 Running
// 态条目现算耗时;Ok/Error 态用各自的 end_time,ticker 多久没醒都不影响
// 收尾行的耗时数字(一旦 Finish() 过就定住了);token 数同理,Running 态
// 现算到目前为止的累计,Ok/Error 态定住 Finish() 那一刻的累计值。
//
// theme.reset 为空(plain 主题)时首行状态灯换成方括号文字
// ([RUNNING]/[OK]/[ERROR]),跟 cli::TranscriptStatusWord 的 plain 规矩
// 一致,摘要行/提示行不着色;彩色主题下首行状态灯是跟 transcript 同一个
// 圆点字符,摘要行/提示行统一用 theme.stats 那个淡色。
std::vector<std::string> FormatAgentStatusLines(const std::vector<AgentStatusEntry>& entries,
                                                  std::chrono::steady_clock::time_point now,
                                                  const Theme& theme);

}  // namespace lubancode::cli
