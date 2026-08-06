// 平台抽象层:主目录、环境变量、字符编码转换(v0.20.x 跨平台单)。
//
// 接口从现有 Windows 代码提炼——config.cpp 的 GetEnv/HomeDir、
// tools/process_exec.cpp 的 Utf8ToWide/WideToUtf8/AcpBytesToUtf8 原样搬进
// 来,调用点改 include 这里,#ifdef 收拢到 platform/ 内部。
//
// 宽窄转换(Utf8ToWide 那三个)只在 Windows 有意义——POSIX 的路径、argv、
// 终端天生就是字节串,程序内部统一按 UTF-8 处理,不需要转码,所以这三个
// 函数只在 _WIN32 下声明,业务代码用到它们的地方本来就在 #ifdef _WIN32
// 分支里(拼 CreateProcessW 命令行之类)。
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace lubancode::platform {

// 读一个环境变量;没设置或者是空串都算"没有"。Windows 走 _dupenv_s(MSVC
// 下 std::getenv 会吃 C4996 告警),其余平台走 std::getenv。
std::optional<std::string> GetEnvVar(const char* name);

// 用户主目录:Windows 取 %USERPROFILE%,别的平台取 $HOME。找不到返回
// std::nullopt。
std::optional<std::string> HomeDir();

// 当前可执行文件的绝对路径。官方 skills 要跟发行包/安装前缀走，不能拿
// cwd 猜；找不到时返回 nullopt，主程序照常运行，只是没有官方技能层。
std::optional<std::filesystem::path> ExecutablePath();

// 官方技能目录：先找 <exe-dir>/skills（便携包、Windows 安装与开发构建），
// 再找 <prefix>/share/lubancode/skills（POSIX install.sh/CMake install）。
std::optional<std::string> OfficialSkillsDir();

// 把 source 原子换到 destination。两条路径须在同一文件系统；成功后
// source 不复存在。memory/index 这类“先写临时文件，再整份替换”的路径
// 共用它，免得 Windows 的 rename 不能覆盖目标、POSIX 却能覆盖，业务层
// 各写一套分叉。
std::expected<void, std::string> ReplaceFileAtomically(const std::filesystem::path& source,
                                                        const std::filesystem::path& destination);

#ifdef _WIN32

std::wstring Utf8ToWide(const std::string& utf8);
std::string WideToUtf8(const std::wstring& wide);

// cmd.exe 内置命令走系统 ANSI 代码页(国内机器是 GBK)往管道里写字节,这个
// 转换只对"命令经 cmd.exe 跑"的路径需要——具体原因见 run_command.cpp 里
// 原来那段注释(chcp 对重定向管道不起作用,实测过)。
std::string AcpBytesToUtf8(const std::string& acp_bytes);

#endif

}  // namespace lubancode::platform
