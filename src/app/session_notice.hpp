// 非 turn 事件的会话通知 sink(骨架拆解反弹·问题 2)。
//
// 病:后台命令跑完、后台子代理回流这两类"系统侧播报"原先在
// TerminalSessionController::Run 里直接 TermOut() 打终端——不走任何 sink,
// 事件流(ServerEvent/JsonEventSink)看不见它们,app-server/脚本桥那路
// 前端永远收不到"后台那条命令完事了"。
//
// 修:立一只窄接口 SessionNoticeSink。控制器把通知折成 SessionNotice 递
// 进来;终端实现(TerminalSessionNoticeSink)照原先的画法逐字节上屏
//(行为零变),往后 app-server 直出时再往这挂第二只 sink,不用回头改
// 控制器。通知不插进对话流(不发给模型、不消耗 token)——只给人看,
// 这条产品语义随结构一起钉在这。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cli/theme.hpp"
#include "tools/background_tasks.hpp"  // BackgroundTaskStatus

namespace lubancode::app {

// 一枚系统侧通知。kind 定形状,两类载荷分字段放(谁的不用谁)。
struct SessionNotice {
    enum class Kind {
        BackgroundTaskDone,   // 后台命令任务翻入终态(完成/失败/已停止/停止失败)
        SubagentCompletion,   // 后台子代理结果回流,轮前短进度行
    };

    Kind kind = Kind::BackgroundTaskDone;

    // ---- BackgroundTaskDone 的载荷 ----
    std::string task_id;  // 台账的字符串任务号(与 BackgroundTaskInfo 同型)
    lubancode::tools::BackgroundTaskStatus status = lubancode::tools::BackgroundTaskStatus::Completed;
    std::optional<int> exit_code;  // 非完成态给(unknown = 空)
    std::string command;

    // ---- SubagentCompletion 的载荷 ----
    std::string title;               // 通知标题(与 transcript 事件同一条文案)
    std::vector<std::string> notes;  // 逐条短行(每只子代理一行)
};

class SessionNoticeSink {
public:
    virtual ~SessionNoticeSink() = default;
    // 在调用方(会话主循环)线程上被调;实现自己管同步与落笔。
    virtual void Emit(const SessionNotice& notice) = 0;
};

// 终端实现:画法自 controller 原先的直接 TermOut 段逐字搬来(颜色、
// "(exit N)" 段、缩进行、锁内单条的规矩全部照旧)——改道前后终端
// 输出一个字节不差。
class TerminalSessionNoticeSink final : public SessionNoticeSink {
public:
    explicit TerminalSessionNoticeSink(const lubancode::cli::Theme& theme) : theme_(theme) {}

    void Emit(const SessionNotice& notice) override;

private:
    const lubancode::cli::Theme& theme_;
};

}  // namespace lubancode::app
