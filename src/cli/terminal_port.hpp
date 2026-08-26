// 终端接线收尾单:终端输出端口。
//
// 病灶三(用户查账原文):终端输出出口未统一——interactive_session 约
// 290 处、turn_runner 约 60 处直接 std::cout/std::cerr 散打。立这一只
// 端口,大类与命令 presenter 的 stdout/stderr 写全改走它;往后要改道
//(TUI 帧缓冲、测试捕获、录制)只动这一处,不再满仓追 std::cout。
//
// 边界与锁:
//   - 端口只管"流往哪儿",不管"什么时候写"——流式期间与监听线程/心跳
//     线程错开的 StdoutWriteMutex 规矩不变,锁照旧由调用方拿(拿法与
//     直接写 std::cout 时一字不差);
//   - 默认指 stdout/stderr;Redirect 供测试把两只流都换成 ostringstream,
//     Reset 收回。单进程一只(TermPort()),与 console_input 的
//     SharedEditor/SessionSteeringQueue 同一款进程级单例的存法。
//   - cli 不反引 app;runtime/agent/tools 不引 cli——这只头只认标准库,
//     哪层都能安全地链。

#pragma once

#include <ostream>

namespace lubancode::cli {

// 输出端口本体:两枚流指针 + 改道口。不拥有流(默认指 std::cout/
// std::cerr;Redirect 借调用方的流,寿命归调用方管)。
class TerminalPort {
public:
    TerminalPort() = default;

    // 主输出流(会话正文、命令回显、统计行)。与 std::cout 同构,链式写、
    // flush 都照旧。
    std::ostream& out() { return *out_; }
    // 错误/告警流(与 stdout 错开的那几路:图片读取失败、钩子阻断……)。
    std::ostream& err() { return *err_; }

    // 测试改道:两只流都借引用存指针;传 nullptr 的那只保持原样。
    void Redirect(std::ostream* out, std::ostream* err);
    // 收回默认(stdout/stderr)。
    void Reset();

private:
    std::ostream* out_ = &StdOut();
    std::ostream* err_ = &StdErr();

    static std::ostream& StdOut();
    static std::ostream& StdErr();
};

// 进程级端口实例。会话、命令 presenter、turn_runner 全从这两枚口出水。
TerminalPort& TermPort();

// 便捷口:写起来与 std::cout/std::cerr 一模一样,替换散打时机械换名即可。
//   TermOut() << theme.stats << line << theme.reset << "\n";
std::ostream& TermOut();
std::ostream& TermErr();

}  // namespace lubancode::cli
