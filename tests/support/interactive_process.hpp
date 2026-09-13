// 交互式子进程夹具(测试专用)——工业化多协议接入单 P1 真进程冒烟的
// 底座(§17.4"真进程协议测试:启动真正发行入口、连接 stdio")。
//
// 与 platform::RunProcessWithStdin 的差别:那次性把 stdin 写完关管
//(EOF 即收线,回合没跑完就被打断);真协议客户端要"发一行、等事件、
// 再发一行"——turn/completed 到手后才许 shutdown。本夹具保住写端,
// 逐行读 stdout(stderr 单独收,不掺协议流)。
//
// 章法(与 FakeHttpServer 同款纪律):
//   - 只在测试二进制里链接,不进生产库;
//   - 读线程独立(detach 语义,进程退出后线程按共享 state 收口,不悬垂);
//   - 析构若没显式收线:杀树(Windows Job Object 一锅端;POSIX 进程组
//     SIGKILL),不留僵尸。
#pragma once

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::test_support {

class InteractiveProcess {
public:
    // 起进程。argv[0] 是可执行文件;extra_env 追加(已有同名变量被覆盖);
    // cwd_utf8 空 = 继承。失败回 nullptr(原因进 error)。
    static std::unique_ptr<InteractiveProcess> Spawn(const std::vector<std::string>& argv,
                                                     const std::vector<std::pair<std::string, std::string>>& extra_env,
                                                     const std::string& cwd_utf8, std::string* error);
    ~InteractiveProcess();

    InteractiveProcess(const InteractiveProcess&) = delete;
    InteractiveProcess& operator=(const InteractiveProcess&) = delete;

    // 写一行进子进程 stdin(自带换行)。false = 管道已断。
    bool WriteLine(const std::string& line);
    // 关 stdin 写端(子进程读到 EOF)。
    void CloseStdin();

    // 等下一行 stdout(剥 \r\n)。nullopt = 超时或流尽。
    std::optional<std::string> ReadLine(int timeout_ms);
    // stderr 的已收文本快照(诊断用,不掺协议流)。
    std::string StderrText();

    // 等进程退出;true = 已退,exit_code 有效。
    bool Wait(int timeout_ms, int* exit_code);

private:
    InteractiveProcess() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace lubancode::test_support
