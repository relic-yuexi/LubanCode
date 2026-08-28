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
#include <string_view>
#include <vector>

namespace lubancode::platform {

// UTF-8 字符串 <-> std::filesystem::path 的正统通道(宽窄转换异常单)。
// Windows 上 path 的窄口走系统 ANSI 代码页(国内机器是 GBK):路径带
// emoji/生僻字时 path::string() 会抛 system_error,what() 正是
// "No mapping for the Unicode character exists in the target multi-byte
// code page."(ERROR_NO_UNICODE_TRANSLATION,1113)——真机上掐死过整场
// 会话;窄串构造 path 也会按 GBK 误解 UTF-8 字节。全仓凡 path 要落
// UTF-8 文本/日志/请求的,一律走 PathToUtf8;确需 ACP 的 Win32 边界,
// 注释写明再碰 .string()。
inline std::filesystem::path Utf8ToPath(const std::string& utf8) {
    const std::u8string_view view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(view);
}

inline std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

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

// 官方 Package 目录（统一 Package 封装单四层扫描的 official 层）：探法与
// OfficialSkillsDir 同款，只是名字换 packages。找不到返回 nullopt，主程
// 序照常运行——official 层缺席只少一层扫描。
std::optional<std::string> OfficialPackagesDir();

// 把 source 原子换到 destination。两条路径须在同一文件系统；成功后
// source 不复存在。memory/index 这类“先写临时文件，再整份替换”的路径
// 共用它，免得 Windows 的 rename 不能覆盖目标、POSIX 却能覆盖，业务层
// 各写一套分叉。
std::expected<void, std::string> ReplaceFileAtomically(const std::filesystem::path& source,
                                                        const std::filesystem::path& destination);

// 当前工作目录的 UTF-8 写法。两平台同一套 std::filesystem,内联在此,
// 不再各写一份。原本住在 main.cpp 的匿名命名空间里,app 层要用,先搬来。
inline std::string CurrentDirUtf8() {
    const std::filesystem::path cwd = std::filesystem::current_path();
    const std::u8string u8 = cwd.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

#ifdef _WIN32

std::wstring Utf8ToWide(const std::string& utf8);
std::string WideToUtf8(const std::wstring& wide);

// cmd.exe 内置命令走系统 ANSI 代码页(国内机器是 GBK)往管道里写字节,这个
// 转换只对"命令经 cmd.exe 跑"的路径需要——具体原因见 run_command.cpp 里
// 原来那段注释(chcp 对重定向管道不起作用,实测过)。
std::string AcpBytesToUtf8(const std::string& acp_bytes);

// 按给定代码页把字节解成 UTF-8;字节在该代码页里解不动(非法序列)返回
// std::nullopt。与 AcpBytesToUtf8 的差别:那份"尽力而为、失败原样退回",
// 这份要明确成败——hooks 子进程流的解码拿它当"这一页解不解得动"的判定,
// 不许闷头猜。
std::optional<std::string> CodePageBytesToUtf8(unsigned int code_page, const std::string& bytes);

// hooks 子进程 stdout/stderr 的候选代码页:控制台输出页在前(PowerShell
// 5.1 重定向 stderr 走的就是它),系统 ANSI 页在后(cmd.exe 路径)。解码
// 顺序仍是"先认 UTF-8",这两页只是明示的次选,命中即标注。
std::vector<unsigned int> ChildStreamCodePageCandidates();

#endif

}  // namespace lubancode::platform
