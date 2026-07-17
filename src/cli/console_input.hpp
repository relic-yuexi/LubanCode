// 全程序唯一的 stdin 读入口。存在的理由是绕开一个 Windows 老毛病:
// main.cpp 里 SetConsoleCP(CP_UTF8) 之后,窄字符 std::getline(std::cin, ...)
// 读中文,在真控制台(conhost)下会跟 ReadFile 的 CP_UTF8 支持撞车——
// typed 的多字节字符偶发读空或读乱,尤其是几次 ReadFile 交替调用之后
// (交互模式里"主提示符读一行"跟"工具确认读一行"正好就是这种交替)。
// 见 console_input.cpp 开头注释,写了实测结论。
//
// M6.5 把真控制台这条路从"整行读入(ReadConsoleW)"升级成"逐键输入编辑器"
// (核心逻辑在 cli/line_editor.hpp 的 LineEditorCore,不认 Win32,可单测;
// 这里只是拿真实按键喂它、按它吐出来的 RenderState 重画屏幕),换来方向键
// 移光标、上下键翻历史、Tab 补全 slash 命令、Shift+Tab 循环切确认模式这些
// 花活。管道/重定向场景完全不受影响,还是走最下面的 std::getline 老路,
// 一个字节都没改。

#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cli/line_editor.hpp"
#include "cli/theme.hpp"

namespace lubancode::cli {

// 打印 prompt(不含换行,可传空串跳过打印),读一行输入。
// Windows 下 stdin 是真控制台(GetFileType == FILE_TYPE_CHAR)时,走逐键
// 输入编辑器(方向键/历史/Tab 补全/Shift+Tab 切模式都在这条路上);
// stdin 是管道/重定向文件时,回退到 std::getline(保住
// `echo "x" | lubancode.exe` 这种自动化用法和集成测试,行为跟升级前完全
// 一致)。统一剥掉行尾的 \r\n。EOF(Ctrl+Z/Ctrl+D 或管道读尽)返回
// std::nullopt。
//
// theme 只用来给 slash 补全提示行、Shift+Tab 模式切换通知上色;不传就是
// 默认构造的空 Theme(没有颜色转义,不影响功能,只是没有颜色)。
//
// esc_rejects:M10 新增,给 [y/a/N] 确认提示用。true 时,按下 Esc 不再走
// "清空当前行、留在同一次读取里继续等"那条空闲编辑态的路,而是立刻当这一次
// 读取提交了一个空串返回——main.cpp 的确认回调本来就把"不是 y/Y/a/A"的
// 任何答案都当拒绝,空串自然而然就是拒绝,不用另外加一条判断分支。默认
// false,不影响其余调用点(主提示符、/model 选择……)的行为。
std::optional<std::string> ReadLine(const std::string& prompt, const Theme& theme = Theme{},
                                     bool esc_rejects = false);

// 会话级确认模式的查询/设置。真控制台下 Shift+Tab 会改这个状态(存在
// ReadLine() 内部维护的、贯穿整条交互会话的 LineEditorCore 实例里,见
// console_input.cpp 的 SharedEditor());main.cpp 的工具确认回调、主提示符
// 前缀都读这个。--yes 等价于启动时调一次
// SetConfirmMode(ConfirmMode::Yolo)。管道/重定向模式下这两个函数依然可用
// (状态本身不依赖真实控制台),只是永远不会被 Shift+Tab 改变——管道场景
// 根本读不到"按键",只有整行文本。
ConfirmMode CurrentConfirmMode();
void SetConfirmMode(ConfirmMode mode);

// M11(0.10.0):真控制台此刻的显示宽度(列数),给分界线(cli::BuildDividerLine)
// 探测用。查的是 stdout 那个句柄(跟 DetectConsoleCapability 一致)——分界线
// 关心的是"要打印到哪儿",不是 stdin。探测不到(非真控制台、GetConsoleScreenBufferInfo
// 失败……)返回 std::nullopt,调用方按 80 列兜底。非 Windows 平台恒返回
// std::nullopt。
std::optional<int> DetectConsoleWidth();

// M10:main.cpp 的流式回调(打字机输出 on_text_delta、on_tool_start……)在
// 主线程上打印;TurnInputListener 的 "[已打断]"/"[已排队] ..." 提示在监听
// 线程上打印——两边都写 std::cout,不加锁会在真终端上偶发交错、把画面
// 弄花。main.cpp 里凡是"流式期间"(Run() 还没返回)可能触发的 std::cout
// 写,都拿这把锁包一下,跟监听线程互斥。管道模式下监听线程压根不会起,
// 锁永远拿得到,不影响非交互路径的性能/行为。
std::mutex& StdoutWriteMutex();

// M10:ESC 打断当前轮 + 消息排队用的监听器。main.cpp 在"发出请求到本轮
// Run() 结束"这段窗口期起一个实例:ESC 键按下就把 cancel_flag 置位、打一行
// 淡色 "[已打断]";其余可打印字符进内部排队缓冲(Backspace 能退格),遇
// Enter 就把整行（非空才算）落进队列、打一行淡色 "[已排队] <内容>"。
//
// 跟 SharedEditor() 那条"真正在读一行"的路径靠一把互斥锁
// (ConsoleReadMutex,console_input.cpp 内部静态,两边共用同一份)自动错开
// ——监听线程只在抢到锁的间隙才调 ReadConsoleInputW,ReadLineKeyByKey()
// 整个调用期间一直攥着锁,监听线程那段时间只能干等,绝不会跟"编辑器正在
// 读"的窗口期抢同一份控制台输入(工具确认提示 [y/a/N] 走的也是
// ReadLineKeyByKey,天然享受同样的互斥,不用另外接管)。
//
// stdin 不是真控制台(管道/重定向)时,构造函数直接不起线程,Stop()/
// TakeQueuedLines() 都是安全的空操作——管道场景本来就读不到"按键",这整个
// 类形同虚设,是刻意的、跟 ReadLine() 的管道回退逻辑对齐的设计。
class TurnInputListener {
public:
    TurnInputListener(std::atomic<bool>& cancel_flag, const Theme& theme);
    ~TurnInputListener();

    TurnInputListener(const TurnInputListener&) = delete;
    TurnInputListener& operator=(const TurnInputListener&) = delete;

    // 停止监听、join 线程。main.cpp 在本轮 Run() 返回之后立刻调一次,保证
    // 下一次 ReadLine()(排队消息回显那个 "> " 提示,或者下一轮主提示符)
    // 开始之前,监听线程已经彻底退出——不依赖两边抢互斥锁的运气,干净收尾。
    // 幂等,重复调用/析构时再调都安全。
    void Stop();

    // 取走这次监听期间排队攒下的整行输入,按落队的原始顺序。Stop() 之后
    // 调,取完队列内部清空,不会重复吐给下一轮。
    std::vector<std::string> TakeQueuedLines();

private:
    void ThreadMain();

    std::atomic<bool>& cancel_flag_;
    const Theme& theme_;
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    bool enabled_ = false;
    std::mutex queue_mutex_;
    std::vector<std::string> queued_lines_;
};

}  // namespace lubancode::cli
