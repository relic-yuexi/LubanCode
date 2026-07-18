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

// 纯渲染:把 entries 排成要打印的那几行(一条一行,不带尾随 \n)。now 只用
// 于 Running 态条目现算耗时;Ok/Error 态用各自的 end_time,ticker 多久没
// 醒都不影响收尾行的耗时数字(一旦 Finish() 过就定住了)。
//
// theme.reset 为空(plain 主题)时不着色,状态灯换成方括号文字
// ([RUNNING]/[OK]/[ERROR]),跟 cli::TranscriptStatusWord 的 plain 规矩
// 一致;彩色主题下状态灯是跟 transcript 同一个圆点字符,配色也复用同一
// 套语义色(运行中 tool_line、成功 prompt、失败 error)。
std::vector<std::string> FormatAgentStatusLines(const std::vector<AgentStatusEntry>& entries,
                                                  std::chrono::steady_clock::time_point now,
                                                  const Theme& theme);

}  // namespace lubancode::cli
