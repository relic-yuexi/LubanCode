// Windows 子进程执行的公共基建:CreateProcessW + Job Object(超时连带子子
// 进程一起杀掉)+ 管道捕获合并输出。run_command 工具(M4)、hooks 系统
// (M9)共用同一份实现——原来只有 run_command.cpp 一家在用,M9 加 hooks 时
// 需要同一套"起进程、等它跑完或超时、拿回输出"的机制,抽出来免得复制粘贴
// 一份几乎一样的代码。
//
// 只管"起一个进程"这件事本身;命令行怎么拼(PowerShell 的
// -EncodedCommand,还是 cmd 的 /d /s /c)由调用方决定——run_command.cpp 里
// PowerShell 专属的 Base64Encode/BuildEncodedCommand 没挪过来,hooks 不需要
// PowerShell 那条路,直接走 cmd.exe。
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace lubancode::tools {

struct ProcessResult {
    std::string output;              // 合并捕获的 stdout/stderr,原始字节
    unsigned long exit_code = 0;
    bool timed_out = false;
    bool spawn_failed = false;
    bool output_truncated = false;   // 输出超过上限被截断,进程(连带 Job)已被强制终止
    std::string spawn_error;
};

// 单次执行捕获输出的默认上限(字节)。超过就截断、杀掉整个 Job,防止一条
// 命令把内存吃光(比如 ping -t、dir /s C:\)。
constexpr std::size_t kDefaultMaxOutputBytes = 2 * 1024 * 1024;

#ifdef _WIN32

std::wstring Utf8ToWide(const std::string& utf8);
std::string WideToUtf8(const std::wstring& wide);

// cmd.exe 内置命令走系统 ANSI 代码页(国内机器是 GBK)往管道里写字节,这个
// 转换只对"命令经 cmd.exe 跑"的路径需要——具体原因见 run_command.cpp 里
// 原来那段注释(chcp 对重定向管道不起作用,实测过)。
std::string AcpBytesToUtf8(const std::string& acp_bytes);

// 拼一条 `cmd.exe /d /s /c "<command>"` 命令行,原样交给 cmd 执行,不做任何
// 代码页预处理。
std::wstring BuildCmdCommandLine(const std::string& user_command_utf8);

// 起一个子进程(cmdline 是完整的"可执行文件 + 参数",调用方拼好),合并
// 捕获 stdout/stderr,超时就连同它派生出的子子进程一起杀掉(Job Object 的
// KILL_ON_JOB_CLOSE)。
//
// extra_env 是要临时注入子进程环境的键值对(UTF-8,同名覆盖当前进程环境)。
// 实现上是"临时 SetEnvironmentVariableW -> 起子进程(子进程创建时拿到父
// 进程环境的一份快照)-> 立刻还原"这个套路,不是手搭一份独立环境块——这
// 要求调用方是单线程顺序调用(tools 层的工具执行、钩子执行本来就是顺序
// 跑的,见 agent/loop.cpp,不存在并发覆盖的风险)。
//
// max_output_bytes:捕获输出的上限,超过就截断保留前段、置 output_truncated
// 并强制终止整个 Job。测试用小值,生产路径用默认值即可。
ProcessResult RunProcess(const std::wstring& cmdline, int timeout_ms,
                          const std::vector<std::pair<std::string, std::string>>& extra_env = {},
                          std::size_t max_output_bytes = kDefaultMaxOutputBytes);

#else

// 非 Windows 平台没实现,调用方按 spawn_failed 处理。
ProcessResult RunProcess(const std::wstring& cmdline, int timeout_ms,
                          const std::vector<std::pair<std::string, std::string>>& extra_env = {},
                          std::size_t max_output_bytes = kDefaultMaxOutputBytes);

#endif

}  // namespace lubancode::tools
