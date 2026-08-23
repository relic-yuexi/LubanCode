// 后台任务注册表:进程级单例,管所有 run_in_background 起出来的后台命令。
//
// 进程生命线单(P0/P1)之后的形态:
//   1. 任务台账:每个后台命令登记一条,带 task_id(单调递增字符串 "1"/"2")、
//      命令文本、PID、日志路径、状态(running/stopping/completed/failed/
//      stopped/stop_failed)、退出码(optional:不知道便是 nullopt,绝不借
//      0 冒充成功)、起止时间。
//   2. 完成监控:Register 先建 entry 进表、再起 watcher(次序反过来是 P0
//      竞态:watcher 抢先跑起来在表里找不到自己,任务永远 Running)。
//      watcher 持有 BackgroundProcessHandle 的共享状态,不再拿 PID 猜
//      生死——Wait 阻塞在原生句柄上,退出码由唯一收尸方写进完成态。
//   3. 停止:Stop 落 handle->TerminateTree(Windows 每任务专属 Job 的
//      TerminateJobObject;POSIX 组 SIGTERM→grace→SIGKILL),状态先进
//      Stopping,树死透了才进 Stopped;收不动如实进 stop_failed,不先盖章。
//   4. 输出读取:ReadOutput 按任务读日志尾部 N 行,出口保证合法 UTF-8
//      (按 encoding_hint 决定清洗策略),64KB 尾读从完整换行与 UTF-8 边界
//      起刀,首段被截加标记。
//
// 线程模型:Instance() 是 magic static。entry 持 shared_ptr<BackgroundTaskState>
// (watcher 与 registry 共享,扩容不悬垂);对 entries_ 的读写都在 mutex_ 里。
// 析构(程序退出)时 stop_all_ 置位,watcher 在 Wait 的短周期里醒来自己退出,
// 主线程逐个 join。
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "platform/process.hpp"

namespace lubancode::tools {

enum class BackgroundTaskStatus {
    Running,
    Stopping,     // Stop 已发出,树还没死透
    Completed,    // 进程自己退出,退出码 == 0
    Failed,       // 进程自己退出,退出码 != 0(或退出码未知:不借 0 冒充成功)
    Stopped,      // 被 Stop() 主动收掉,树已死透
    StopFailed,   // Stop 的系统调用链失败,进程可能还活着
};

// 退出码账:不知道便是 nullopt。三处口径(文档/注释/工具输出)收成这一份。
struct BackgroundExit {
    std::optional<int> exit_code;  // WIFEXITED / Windows 精确码
    std::optional<int> signal;     // POSIX 受信号终止(WIFSIGNALED),Windows 恒空
};

struct BackgroundTaskInfo {
    std::string task_id;
    std::string command;
    std::string shell;          // powershell/cmd/sh,描述用,不参与执行
    unsigned long pid = 0;
    std::string log_path;       // 合并 stdout/stderr 的日志文件(UTF-8 路径)
    BackgroundTaskStatus status = BackgroundTaskStatus::Running;
    BackgroundExit exit;        // 终态时填;不知道便是 nullopt
    std::string encoding_hint;  // 日志字节的编码线索(powershell=utf-8 等)
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point finish_time;  // 终态时填
    bool completed_reported = false;  // 这条"新完成"是否已被 DrainCompleted 取走
};

// 人话标签(List/detail/通知共用一份,别再两处各写一套)。
const char* BackgroundTaskStatusLabel(BackgroundTaskStatus s);

class BackgroundTaskRegistry {
public:
    static BackgroundTaskRegistry& Instance();

    // 登记一个已经 spawn 成功的后台任务(run_command 后台分支调)。先建
    // entry 进表、再起 watcher——反过来就是 P0 竞态。watcher 持 handle 的
    // 共享状态探活,不再按 task_id 回表里找自己。
    std::string Register(std::string command, std::string shell, unsigned long pid, std::string log_path,
                         std::shared_ptr<platform::BackgroundProcessHandle> handle = nullptr);

    // 当前所有任务的快照(线程安全拷贝一份)。按 task_id 数字升序。
    std::vector<BackgroundTaskInfo> List();

    // 查单个。找不到返回 nullopt。
    std::optional<BackgroundTaskInfo> Get(const std::string& task_id);

    // 读日志文件尾部 tail_lines 行。task_id 找不到/文件读不了返回空串。
    // tail_lines <= 0 表示读全文(上限 64KB 防爆);>0 时按行切取后 N 行
    // (内部也是先读最后 64KB 再切行,日志再大也不把内存吃光)。
    // 出口保证合法 UTF-8:按 encoding_hint 清洗,64KB 起刀处先退到完整
    // UTF-8 边界与换行边界,首段被截加 [日志前部已省略] 标记。
    std::string ReadOutput(const std::string& task_id, int tail_lines = 50);

    // 杀掉指定任务:落 handle->TerminateTree(整棵树),状态 Stopping ->
    // Stopped;收不动如实进 StopFailed,不先盖章。没有 handle 的旧调用
    // (测试直接 Register)退化成 PID 探活那条老路。
    // 返回是否认得这个 task_id。
    bool Stop(const std::string& task_id);

    // 取走"自上次 drain 以来新进入终态"的任务,按完成先后顺序。调用后这些
    // 任务标 completed_reported=true,不会重复吐。
    std::vector<BackgroundTaskInfo> DrainCompleted();

private:
    BackgroundTaskRegistry() = default;
    ~BackgroundTaskRegistry();
    BackgroundTaskRegistry(const BackgroundTaskRegistry&) = delete;
    BackgroundTaskRegistry& operator=(const BackgroundTaskRegistry&) = delete;

    // watcher 持的共享状态:不进 entries_ 也能安全读写生命周期字段。
    struct TaskState {
        std::mutex mutex;
        BackgroundTaskStatus status = BackgroundTaskStatus::Running;
        BackgroundExit exit;
        std::chrono::system_clock::time_point finish_time{};
        bool completed_reported = false;
        platform::BackgroundProcessHandle::Completion last_completion{};
        bool completion_seen = false;
    };

    // watcher 线程主循环:等在 handle 上(或退化轮询),进程结束就写终态。
    void WatchThread(std::shared_ptr<TaskState> state,
                     std::shared_ptr<platform::BackgroundProcessHandle> handle, unsigned long pid,
                     std::string task_id);

    // 退化探活(没有 handle 的旧调用):alive=true 还活着;false 已结束。
    static bool IsPidAlive(unsigned long pid);

    struct Entry {
        BackgroundTaskInfo info;
        std::shared_ptr<TaskState> state;
        std::shared_ptr<platform::BackgroundProcessHandle> handle;  // 可空(旧调用)
        std::thread watcher;
    };

    std::mutex mutex_;
    std::vector<std::unique_ptr<Entry>> entries_;
    std::atomic<long long> next_id_{1};
    std::atomic<bool> stop_all_{false};
};

}  // namespace lubancode::tools
