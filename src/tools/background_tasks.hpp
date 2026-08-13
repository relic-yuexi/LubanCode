// 后台任务注册表:进程级单例,管所有 run_in_background 起出来的后台命令。
//
// 现状(改造前):run_command 的后台分支 spawn 完就把 PID + 日志路径甩回给模型,
// 之后再也不管——命令跑没跑完、什么时候完、输出长啥样,模型和用户都两眼一抹黑,
// 想看只能自己再起一条普通命令去 tail 日志。这就是"spawn 完就忘"。
//
// 这一层补上三件 Claude Code 的 BashBackground 本来就有的本事:
//   1. 任务台账:每个后台命令登记一条,带 task_id(单调递增字符串 "1"/"2")、
//      命令文本、PID、日志路径、状态(running/completed/failed/stopped)、
//      退出码、起止时间。模型和 slash 命令随时能查。
//   2. 完成监控:登记时起一条 watcher 线程轮询探活(IsPidAlive),进程一结束
//      就记下退出码、标终态;主循环每轮开头 DrainCompleted() 把"新完成"的
//      任务取走,打一行通知给用户看。不阻塞对话流。
//   3. 输出读取:ReadOutput 按任务读日志文件尾部 N 行(模型经 background_output
//      工具调),不用自己拼 tail 命令。
//
// 跨平台探活:
//   - Windows:OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) + GetExitCodeProcess,
//     STILL_ACTIVE 是活着,别的值就是退出码(精确)。
//   - POSIX:后台子进程 setsid() 脱离了 lubancode 的会话,父进程没有 waitpid
//     的权利(不是它的子进程),只能 kill(pid, 0) 探活——活着/死了能分清,
//     退出码拿不到(进程结束信息已被内核收走),exit_code 标 -1(未知)。
//   - 两边都有 PID 复用的理论风险(进程死后 PID 被新进程占用),但 watcher
//     在 spawn 返回的瞬间就起、200ms 一探,落到这个窗口里的概率极低,不额外
//     对冲(对冲要长持句柄,得改 platform 层 spawn 签名,破坏面太大)。
//
// 线程模型:Instance() 是 magic static(线程安全初始化)。entries_ 每条带自己
// 的 watcher 线程;对 entries_ 的所有读写都在 mutex_ 里。watcher 线程通过
// task_id 在锁里找自己的 entry(不持有 Entry* 指针,vector 扩容也不怕悬垂)。
// 析构(程序退出)时 stop_all_ 置位,所有 watcher 在 200ms 内自己退出,主线程
// 逐个 join——join 不持锁,watcher 最后一次抢锁释放后检查 stop_all_ 即返回,
// 不死锁。
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace lubancode::tools {

enum class BackgroundTaskStatus {
    Running,
    Completed,    // 进程自己退出,退出码 == 0
    Failed,       // 进程自己退出,退出码 != 0
    Stopped,      // 被 Stop() 主动杀掉
};

struct BackgroundTaskInfo {
    std::string task_id;
    std::string command;
    std::string shell;          // powershell/cmd/sh,描述用,不参与执行
    unsigned long pid = 0;
    std::string log_path;       // 合并 stdout/stderr 的日志文件(UTF-8 路径)
    BackgroundTaskStatus status = BackgroundTaskStatus::Running;
    int exit_code = 0;          // 完成后填;POSIX 脱离子进程拿不到,标 -1
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point finish_time;  // 终态时填
    bool completed_reported = false;  // 这条"新完成"是否已被 DrainCompleted 取走
};

class BackgroundTaskRegistry {
public:
    static BackgroundTaskRegistry& Instance();

    // 登记一个已经 spawn 成功的后台任务(run_command 后台分支调)。起一条
    // watcher 线程轮询探活。返回单调递增的 task_id("1"/"2"/...)。
    std::string Register(std::string command, std::string shell, unsigned long pid, std::string log_path);

    // 当前所有任务的快照(线程安全拷贝一份)。按 task_id 数字升序。
    std::vector<BackgroundTaskInfo> List();

    // 查单个。找不到返回 nullopt。
    std::optional<BackgroundTaskInfo> Get(const std::string& task_id);

    // 读日志文件尾部 tail_lines 行。task_id 找不到/文件读不了返回空串。
    // tail_lines <= 0 表示读全文(上限 64KB 防爆);>0 时按行切取后 N 行
    // (内部也是先读最后 64KB 再切行,日志再大也不把内存吃光)。
    std::string ReadOutput(const std::string& task_id, int tail_lines = 50);

    // 杀掉指定任务。Windows 上 TerminateProcess 根进程;POSIX 上
    // kill(-pid, SIGTERM) 杀整个进程组(后台子进程 setsid 后 pid 就是 pgid)。
    // 已终态的任务不重复杀。返回是否认得这个 task_id(true 不保证真杀掉,
    // 只保证表里有这一条)。
    bool Stop(const std::string& task_id);

    // 取走"自上次 drain 以来新进入终态"的任务,按完成先后顺序。调用后这些
    // 任务标 completed_reported=true,不会重复吐。主循环每轮开头调一次,
    // 有内容就打一行"[后台任务 #N 完成]"通知给用户。
    std::vector<BackgroundTaskInfo> DrainCompleted();

private:
    BackgroundTaskRegistry() = default;
    ~BackgroundTaskRegistry();
    BackgroundTaskRegistry(const BackgroundTaskRegistry&) = delete;
    BackgroundTaskRegistry& operator=(const BackgroundTaskRegistry&) = delete;

    // watcher 线程主循环:轮询探活,进程结束就锁住表标终态。
    void WatchThread(std::string task_id);

    // 跨平台探活。alive=true 表示还活着;alive=false 表示已结束,exit_code_out
    // 填退出码(Windows 精确,POSIX 拿不到填 -1)。
    static bool IsPidAlive(unsigned long pid, int& exit_code_out);

    struct Entry {
        BackgroundTaskInfo info;
        std::thread watcher;
    };

    std::mutex mutex_;
    std::vector<std::unique_ptr<Entry>> entries_;
    std::atomic<long long> next_id_{1};
    std::atomic<bool> stop_all_{false};
};

}  // namespace lubancode::tools
