// Windows 实现:一次性捕获搬自 tools/process_exec.cpp(CreateProcessW +
// Job Object + 管道捕获,逻辑一字未改);长命双向管道搬自
// mcp/transport.cpp(lsp/transport.cpp 那份与之几乎相同,合并时取了 MCP
// 版——它的 JoinReaderThreads 多一层"限时等待 + CancelSynchronousIo"兜底,
// 对 LSP 只是更稳,不改变正常路径行为)。
#include "platform/process.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <jobapi2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>

#include "platform/paths.hpp"
#include "platform/text_encoding.hpp"  // Utf8PrefixBoundary:输出帽对齐 UTF-8 边界

namespace lubancode::platform {

namespace {

// 输出帽是字节刀,但刀口不许劈进多字节序列的腰里——中文输出恰好在
// limit 上断成 0xE5 开头的半截汉字,这坨字节流到 nlohmann::json 序列化
// 就是 type_error.316(0.26.41 真机崩的根因)。截断后对齐到码点边界。
// 两条路:
//   - output 超过帽(WithStdin 全量攒再置旗那路):Utf8PrefixBoundary
//     正常语义,退到截断点前的整字边界。
//   - output 恰好满帽(主路径 reader 的 take 填满即停):offset ==
//     size 时帮手直接返回 size,得换姿势——先看整段是否合法,非法且
//     首个坏字节落在"去掉最后一字节的安全前缀"之外(即只在尾巴上),
//     才退到安全前缀。正文中间就有坏字节的(整段 GBK 那类)不动——
//     清洗归调用方(run_command 的 SanitizeUtf8 那道关),这里只治
//     "自己这刀劈出来的悬空"。
void AlignOutputToUtf8Boundary(std::string& output, std::size_t max_output_bytes) {
    if (output.size() > max_output_bytes) {
        output.resize(Utf8PrefixBoundary(output, max_output_bytes));
        return;
    }
    if (output.size() < 2 || output.size() != max_output_bytes) {
        return;  // 没满帽:整段完整交付,悬空只可能来自子进程自己,不归这刀管
    }
    const std::size_t first_bad = FirstInvalidUtf8Offset(output);
    if (first_bad == std::string::npos) {
        return;  // 满帽且合法:刀口恰好落在字缝上
    }
    const std::size_t tail_safe = Utf8PrefixBoundary(output, output.size() - 1);
    if (first_bad >= tail_safe) {
        output.resize(tail_safe);  // 坏在尾巴:退掉悬空的半截字
    }
}

// Replace 模式的显式环境块:"KEY=VALUE\0KEY=VALUE\0\0" 的 UTF-16 串,交给
// CreateProcessW 的 lpEnvironment——宿主环境一概不递(plugins 单第 8 步
// 的最小环境硬保证)。空表 = 子进程一个变量都没有。
std::wstring BuildExplicitEnvironment(const EnvPairs& env) {
    std::wstring block;
    for (const auto& [key, value] : env) {
        block += Utf8ToWide(key);
        block += L'=';
        block += Utf8ToWide(value);
        block += L'\0';
    }
    block += L'\0';  // 块尾双 \0
    return block;
}

// 显式环境块(Inherit 模式,进程生命线单 P0 的并发修复):从父进程环境
// 拷一份,按 Windows 大小写不敏感规则合并 extra_env,拼成排序过的
// UTF-16 environment block 交给 lpEnvironment + CREATE_UNICODE_ENVIRONMENT。
// 绝不改父进程环境——两只 Hook 同拍起进程也不会串值,宿主里其他线程也
// 看不见临时变量。保住 "=C:" 这类 drive-current-directory 特殊项(它们
// 没有值段,key 以 '=' 开头)。键名含 NUL/非法 '='、值含 NUL 一律拒绝,
// 调用方收 spawn_failed。
std::wstring BuildMergedEnvironmentBlock(const EnvPairs& extra_env, std::string* error_out) {
    // 1) 父环境快照:GetEnvironmentStringsW 自带快照语义,不碰父进程。
    std::vector<std::wstring> raw_entries;
    const LPWCH parent_env = GetEnvironmentStringsW();
    if (parent_env != nullptr) {
        for (LPWCH p = parent_env; *p != L'\0'; p += wcslen(p) + 1) {
            raw_entries.emplace_back(p);
        }
        FreeEnvironmentStringsW(parent_env);
    }
    // 2) extra_env 预检:键名带 NUL/空、含 '='(Windows 键名不许),值带 NUL。
    for (const auto& [k, v] : extra_env) {
        if (k.empty() || k.find('=') != std::string::npos || k.find('\0') != std::string::npos) {
            if (error_out != nullptr) {
                *error_out = "环境变量名非法(空/含 '=' 或 NUL): " + k;
            }
            return std::wstring();
        }
        if (v.find('\0') != std::string::npos) {
            if (error_out != nullptr) {
                *error_out = "环境变量值含 NUL: " + k;
            }
            return std::wstring();
        }
    }
    // 3) 合并:普通项拆成 (key, value),extra_env 的键按大小写不敏感规则
    // 覆盖父环境同键;"=C:" 这类 drive-current-directory 特殊项(key 以
    // '=' 开头)没有值段,原样整串保留,不参与覆盖。
    std::vector<std::pair<std::wstring, std::wstring>> merged;  // (key原样, value;特殊项 value 为空串且带 special 标记)
    merged.reserve(raw_entries.size() + extra_env.size());
    std::vector<std::wstring> special_entries;
    for (const std::wstring& entry : raw_entries) {
        const std::size_t eq = entry.find(L'=');
        if (eq == std::wstring::npos || eq == 0) {
            special_entries.push_back(entry);  // "=C:=D:\\path" 或无 '=' 的怪项
            continue;
        }
        merged.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
    }
    for (const auto& [k, v] : extra_env) {
        const std::wstring wk = Utf8ToWide(k);
        const std::wstring wv = Utf8ToWide(v);
        bool replaced = false;
        for (auto& [mk, mv] : merged) {
            if (mk.size() == wk.size() && _wcsicmp(mk.c_str(), wk.c_str()) == 0) {
                mv = wv;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            merged.emplace_back(wk, wv);
        }
    }
    // 4) 排序(键大小写不敏感)后拼块:特殊项不带 '='(原串自带),普通项
    // "KEY=VALUE"。块尾双 \0。
    std::sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) {
        return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0;
    });
    std::wstring block;
    for (const std::wstring& special : special_entries) {
        block += special;
        block += L'\0';
    }
    for (const auto& [k, v] : merged) {
        block += k;
        block += L'=';
        block += v;
        block += L'\0';
    }
    block += L'\0';  // 块尾双 \0
    return block;
}

// Windows 命令行参数的标准转义算法(CommandLineToArgvW 的逆过程):没有
// 空白/引号就原样返回,否则用双引号包起来,内部的反斜杠+引号按规则转义。
// command/args 里任何一段带空格(比如子进程脚本路径含空格)都得靠这个才能
// 被子进程正确解析成一个参数,不会被从中间断开。(搬自 mcp/transport.cpp,
// lsp/transport.cpp 里那份逻辑相同,合并成这一份。)
std::wstring QuoteArgW(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring result = L"\"";
    for (auto it = arg.begin();; ++it) {
        int backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++backslashes;
            ++it;
        }
        if (it == arg.end()) {
            result.append(static_cast<std::size_t>(backslashes) * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            result.append(static_cast<std::size_t>(backslashes) * 2 + 1, L'\\');
            result.push_back(*it);
        } else {
            result.append(static_cast<std::size_t>(backslashes), L'\\');
            result.push_back(*it);
        }
    }
    result.push_back(L'"');
    return result;
}

std::wstring BuildProcessCommandLine(const std::string& command, const std::vector<std::string>& args) {
    std::wstring cmdline = QuoteArgW(Utf8ToWide(command));
    for (const auto& arg : args) {
        cmdline += L' ';
        cmdline += QuoteArgW(Utf8ToWide(arg));
    }
    return cmdline;
}

// ---------------------------------------------------------------------------
// 后台模式(run_command 的 run_in_background)专用小工具。
// ---------------------------------------------------------------------------

// 会话级 Job Object:进程级单例,懒创建,句柄一直攥在主进程手里不主动关。
// 跟 RunProcess/ChildProcess 里那种"一条命令/一条长连接一个 job、用完就关
// 掉杀全家"的临时 job 是两码事——这个 job 只在 lubancode 进程终止时才失效
// (Windows 内核在进程退出时自动关闭它没显式关掉的全部句柄,包括这个),
// KILL_ON_JOB_CLOSE 那一下顺带把挂在上面的所有后台子进程一次杀光,不留
// 孤儿。也正因为靠内核这条强保证,不需要额外注册 atexit/信号处理钩子。
// C++11 起 static 局部变量的初始化本身是线程安全的(magic statics),多个
// 线程并发起后台命令不会重复创建。
HANDLE GetBackgroundSessionJob() {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (j != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
            limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(j, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        }
        return j;
    }();
    return job;
}

// 后台日志文件路径:系统临时目录下,文件名带毫秒时间戳 + 单调计数器,
// 两者叠加保证同一毫秒内并发起多个后台命令也不会撞名。
std::wstring BuildBackgroundLogPathW() {
    static std::atomic<unsigned long long> counter{0};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    const unsigned long long seq = counter.fetch_add(1);
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::wstring filename =
        L"lubancode_bg_" + std::to_wstring(ms) + L"_" + std::to_wstring(seq) + L".log";
    return (dir / filename).wstring();
}

}  // namespace

// ---------------------------------------------------------------------------
// BackgroundProcessHandle(进程生命线单 P0):Windows 每个后台任务一个
// 专属 Job Object + 长持进程句柄。Stop 落 TerminateJobObject(整棵树),
// Wait 落 WaitForSingleObject(句柄,不是 PID),退出码 GetExitCodeProcess
// 精确可读——快进程死了也不怕句柄被系统收走。
// ---------------------------------------------------------------------------

struct BackgroundProcessHandle::Impl {
    HANDLE process = nullptr;  // 长持:进程退出后仍可查询,直到本对象析构
    HANDLE job = nullptr;      // 每任务专属 job:TerminateTree 收整棵树
};

BackgroundProcessHandle::BackgroundProcessHandle() : impl(std::make_unique<Impl>()) {}

BackgroundProcessHandle::~BackgroundProcessHandle() {
    if (impl->job != nullptr) {
        CloseHandle(impl->job);
    }
    if (impl->process != nullptr) {
        CloseHandle(impl->process);
    }
}

bool BackgroundProcessHandle::Wait(int timeout_ms) {
    if (impl->process == nullptr) {
        return true;  // 没起过/已被收口:按已退出算,调用方读完成态
    }
    const DWORD wait_result =
        WaitForSingleObject(impl->process, timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : 0);
    if (wait_result != WAIT_OBJECT_0) {
        return false;
    }
    // 退出:读精确退出码进完成态(一次)。
    std::lock_guard<std::mutex> lock(mutex_);
    if (!completion_known_) {
        DWORD code = 0;
        if (GetExitCodeProcess(impl->process, &code)) {
            completion_.known = true;
            completion_.exit_code = static_cast<int>(code);
        } else {
            // 读不到:如实标未知,绝不借 0 冒充成功。
            completion_.known = false;
        }
        completion_known_ = true;
    }
    return true;
}

BackgroundProcessHandle::Completion BackgroundProcessHandle::Peek() const {
    // 先查一眼(不阻塞),已退出还能顺手把完成态落了。
    if (impl->process != nullptr) {
        const DWORD wait_result = WaitForSingleObject(impl->process, 0);
        if (wait_result == WAIT_OBJECT_0) {
            const_cast<BackgroundProcessHandle*>(this)->Wait(0);
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return completion_;
}

bool BackgroundProcessHandle::IsAlive() {
    if (impl->process == nullptr) {
        return false;
    }
    const DWORD wait_result = WaitForSingleObject(impl->process, 0);
    if (wait_result == WAIT_OBJECT_0) {
        Wait(0);
        return false;
    }
    return true;
}

bool BackgroundProcessHandle::TerminateTree(int grace_ms) {
    // 体面信号 Windows 没有 SIGTERM 那一层;grace_ms 只是给调用方语义上的
    // 宽限(期间树内进程可自行清理——这里不另发 CTRL_BREAK,那需要控制台
    // 共享,后台 NO_WINDOW 进程吃不到)。到点 TerminateJobObject 一锅端。
    if (grace_ms > 0 && IsAlive()) {
        const DWORD wait_result =
            WaitForSingleObject(impl->process, static_cast<DWORD>(grace_ms));
        if (wait_result == WAIT_OBJECT_0) {
            Wait(0);  // 自己退了,收尾完成态
            std::lock_guard<std::mutex> lock(mutex_);
            completion_.terminated_by_stop = true;
            return true;
        }
    }
    bool ok = true;
    if (impl->job != nullptr) {
        if (!TerminateJobObject(impl->job, 1)) {
            ok = false;  // 杀不动:如实报,调用方进 stop_failed 不盖章
        }
    } else if (impl->process != nullptr) {
        if (!TerminateProcess(impl->process, 1)) {
            ok = false;
        }
    }
    if (ok && impl->process != nullptr) {
        WaitForSingleObject(impl->process, 5000);
        Wait(0);
        std::lock_guard<std::mutex> lock(mutex_);
        completion_.terminated_by_stop = true;
    }
    return ok;
}

std::wstring BuildCmdCommandLine(const std::string& user_command_utf8) {
    const std::wstring wide_script = Utf8ToWide(user_command_utf8);
    // cmd /d /s /c "<script>":/d 不跑注册表 AutoRun 项,/s 让外层这对引号
    // 整体当"一段"解析、不逐个匹配内部引号——这是拿 cmd 跑任意命令行
    // (命令里本身可能也带引号)的标准写法。
    return L"cmd.exe /d /s /c \"" + wide_script + L"\"";
}

ProcessResult RunProcess(const std::wstring& cmdline, int timeout_ms, const EnvPairs& extra_env,
                          std::size_t max_output_bytes) {
    return RunProcess(cmdline, timeout_ms, /*cancel=*/nullptr, extra_env, max_output_bytes);
}

ProcessResult RunProcess(const std::wstring& cmdline, int timeout_ms, const std::atomic<bool>* cancel,
                         const EnvPairs& extra_env, std::size_t max_output_bytes,
                         const std::string& cwd_utf8) {
    ProcessResult result;

    // 显式环境块(P0 并发修复):不再临时改宿主环境再还原——Hook
    // dispatcher 给每只 handler 一条线程,老路并发会串值,宿主里其他线程
    // 也会短暂看见临时变量。键值非法(含 NUL/键含 '=')直接 spawn_failed。
    std::string env_error;
    const std::wstring env_block = BuildMergedEnvironmentBlock(extra_env, &env_error);
    if (!env_error.empty()) {
        result.spawn_failed = true;
        result.spawn_error = env_error;
        return result;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        result.spawn_failed = true;
        result.spawn_error = "创建管道失败(错误码 " + std::to_string(GetLastError()) + ")";
        return result;
    }
    // 父进程这边留着的读端不能被子进程继承,不然子进程退出后管道写端还有一份
    // 在父进程手里,ReadFile 会一直等不到 EOF。
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE stdin_null = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = stdin_null;

    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back(L'\0');

    // cwd 走 lpCurrentDirectory(P1 根治,前台半边):不向命令文本拼 cd。
    std::wstring cwd_wide;
    LPCWSTR current_directory = nullptr;
    if (!cwd_utf8.empty()) {
        cwd_wide = Utf8ToWide(cwd_utf8);
        current_directory = cwd_wide.c_str();
    }

    const BOOL ok = CreateProcessW(
        nullptr, cmdline_buf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
        const_cast<LPWSTR>(env_block.c_str()), current_directory, &si, &pi);

    // 子进程已经拿到了自己那份继承来的句柄,父进程这边的可以关了。
    CloseHandle(write_pipe);
    if (stdin_null != nullptr && stdin_null != INVALID_HANDLE_VALUE) {
        CloseHandle(stdin_null);
    }

    if (!ok) {
        CloseHandle(read_pipe);
        result.spawn_failed = true;
        result.spawn_error = "启动子进程失败(错误码 " + std::to_string(GetLastError()) + ")";
        return result;
    }

    // Job Object:进程挂在 CREATE_SUSPENDED 状态先分进 job,再恢复运行,
    // 这样超时时关掉 job 句柄,进程本身和它派生出来的所有子进程一起死,
    // 不会漏杀。
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // overflow_event:读线程发现输出超上限时置信号,主线程的等待立刻醒来
    // 杀进程,不用等超时。手动重置事件,只置一次。
    HANDLE overflow_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    std::string output;
    std::atomic<bool> output_over_limit{false};
    std::atomic<bool> reader_stop{false};
    std::atomic<bool> reader_done{false};
    std::thread reader([&] {
        char buf[4096];
        DWORD n = 0;
        while (!reader_stop.load() && ReadFile(read_pipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
            if (output.size() < max_output_bytes) {
                const std::size_t room = max_output_bytes - output.size();
                const std::size_t take = std::min<std::size_t>(n, room);
                output.append(buf, take);
                // off-by-one 修正:恰好填到 max_output_bytes 不算"超过"。
                // 读到了第 limit+1 个字节(总长超出一字节)才置 overflow。
                if (static_cast<std::size_t>(n) > take && !output_over_limit.load()) {
                    output_over_limit.store(true);
                    if (overflow_event != nullptr) {
                        SetEvent(overflow_event);
                    }
                }
            } else if (!output_over_limit.load()) {
                output_over_limit.store(true);
                if (overflow_event != nullptr) {
                    SetEvent(overflow_event);
                }
            }
            // 超限之后继续读但直接丢弃——不读的话管道缓冲区一满,子进程会
            // 卡在写上死不掉;反正马上就要被杀,读空到 EOF 为止。
        }
        reader_done.store(true);
    });

    // 取消旗是原子布尔,不是内核对象——不能直接进 WaitForMultipleObjects。
    // cancel 非空时把等待切成 100ms 一片,每片醒来查一眼旗(与 POSIX 的
    // 10ms 轮询同语义,粒度放宽到 100ms:ESC 到杀树的延迟人感知不到)。
    // cancel 为空时保持原样的一次性等待。
    const DWORD wait_ms = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : INFINITE;
    HANDLE wait_handles[2] = {pi.hProcess, overflow_event};
    const DWORD wait_count = overflow_event != nullptr ? 2 : 1;
    const auto started_at = std::chrono::steady_clock::now();
    DWORD wait_result = WAIT_FAILED;
    bool hit_cancel = false;
    while (true) {
        if (output_over_limit.load()) {
            wait_result = WAIT_OBJECT_0 + 1;
            break;
        }
        // CreateEventW 在句柄紧张时会失败。那时不能一觉睡满 timeout：
        // 读线程仍会写 output_over_limit，主线程改为短轮询兜底。
        const bool needs_poll = cancel != nullptr || overflow_event == nullptr;
        const DWORD slice = needs_poll && (wait_ms == INFINITE || wait_ms > 100) ? 100 : wait_ms;
        wait_result = WaitForMultipleObjects(wait_count, wait_handles, FALSE, slice);
        if (wait_result == WAIT_OBJECT_0) {
            break;  // 进程退出
        }
        if (cancel != nullptr && cancel->load()) {
            hit_cancel = true;
            break;
        }
        if (wait_result == WAIT_TIMEOUT && slice != INFINITE && slice == wait_ms) {
            break;  // 整段超时到点(cancel 为空的老路径)
        }
        if (wait_ms != INFINITE &&
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at)
                    .count() >= static_cast<long long>(wait_ms)) {
            wait_result = WAIT_TIMEOUT;
            break;
        }
    }
    if (hit_cancel || wait_result == WAIT_TIMEOUT || wait_result == WAIT_OBJECT_0 + 1) {
        // 取消、超时,或者输出超上限:都要把整个 Job 杀干净。
        if (hit_cancel) {
            result.cancelled = true;
        } else if (wait_result == WAIT_TIMEOUT) {
            result.timed_out = true;
        }
        if (job != nullptr) {
            CloseHandle(job);  // KILL_ON_JOB_CLOSE:关句柄的一瞬间,job 里所有进程全杀
            job = nullptr;
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        WaitForSingleObject(pi.hProcess, 5000);
    }

    // 根进程退出不等于后代也退出:后代若继承了管道写端还活着,ReadFile 永远
    // 等不到 EOF,直接 join 会吊死。先把 Job 收掉(连带杀光可能残留的后代),
    // 写端才会全关、读线程才能收尾。
    if (job != nullptr) {
        CloseHandle(job);
        job = nullptr;
    }

    // 兜底(比如 Job 创建/绑定失败过、杀不到后代):限时等读线程,等不到就
    // 取消挂起的 ReadFile,绝不无限期阻塞。
    const auto reader_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!reader_done.load() && std::chrono::steady_clock::now() < reader_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!reader_done.load()) {
        reader_stop.store(true);
        while (!reader_done.load()) {
            CancelSynchronousIo(reader.native_handle());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    reader.join();

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    result.exit_code = exit_code;
    // 截断的刀口对齐 UTF-8 码点边界(见文件头 AlignOutputToUtf8Boundary)。
    AlignOutputToUtf8Boundary(output, max_output_bytes);
    result.output = std::move(output);
    result.output_truncated = output_over_limit.load();

    if (job != nullptr) {
        CloseHandle(job);
    }
    if (overflow_event != nullptr) {
        CloseHandle(overflow_event);
    }
    CloseHandle(read_pipe);
    CloseHandle(pi.hProcess);

    return result;
}

ProcessResult RunProcess(const std::vector<std::string>& argv, int timeout_ms, const EnvPairs& extra_env,
                          std::size_t max_output_bytes) {
    if (argv.empty()) {
        ProcessResult result;
        result.spawn_failed = true;
        result.spawn_error = "argv 不能为空";
        return result;
    }
    return RunProcess(BuildProcessCommandLine(argv[0], std::vector<std::string>(argv.begin() + 1, argv.end())),
                       timeout_ms, extra_env, max_output_bytes);
}

ProcessResult RunProcess(const std::vector<std::string>& argv, int timeout_ms, const std::atomic<bool>* cancel,
                         const EnvPairs& extra_env, std::size_t max_output_bytes, const std::string& cwd_utf8) {
    if (argv.empty()) {
        ProcessResult result;
        result.spawn_failed = true;
        result.spawn_error = "argv 不能为空";
        return result;
    }
    return RunProcess(BuildProcessCommandLine(argv[0], std::vector<std::string>(argv.begin() + 1, argv.end())),
                       timeout_ms, cancel, extra_env, max_output_bytes, cwd_utf8);
}

ProcessResult RunShellCommand(const std::string& command_utf8, int timeout_ms, const EnvPairs& extra_env,
                               std::size_t max_output_bytes) {
    ProcessResult result = RunProcess(BuildCmdCommandLine(command_utf8), timeout_ms, extra_env, max_output_bytes);
    // cmd.exe 走的是系统 ANSI 代码页(国内机器上是 GBK)往管道里写字节,
    // 捕获回来的原始字节要单独转一道才是合法 UTF-8。
    result.output = AcpBytesToUtf8(result.output);
    return result;
}

ProcessResult RunShellCommand(const std::string& command_utf8, int timeout_ms, const std::atomic<bool>* cancel,
                              const EnvPairs& extra_env, std::size_t max_output_bytes,
                              const std::string& cwd_utf8) {
    ProcessResult result =
        RunProcess(BuildCmdCommandLine(command_utf8), timeout_ms, cancel, extra_env, max_output_bytes, cwd_utf8);
    result.output = AcpBytesToUtf8(result.output);
    return result;
}

// RunProcessWithStdin 的公共骨架:与上面 RunProcess(wstring) 同构,多一条
// stdin 管道、一个写线程,并且 stdout/stderr 各走一条管道分开捕获(章法见
// process.hpp 的 RunProcessWithStdin 注释)。写线程阻塞在 WriteFile 上而子
// 进程不读时,靠"超时杀树 → 子进程读端关闭 → WriteFile 以 ERROR_BROKEN_PIPE
// 失败"收场;兜底再 CancelSynchronousIo 一把,绝不吊死。
static ProcessResult RunProcessWithStdinImpl(const std::wstring& cmdline, const std::string& stdin_data,
                                             int timeout_ms, const EnvPairs& extra_env,
                                             std::size_t max_output_bytes) {
    ProcessResult result;
    // 显式环境块(P0 并发修复,与 RunProcess/后台路径同一条):不改宿主
    // 环境。Hook dispatcher 给每只 handler 一条线程,v2 hooks 走这条路。
    std::string env_error;
    const std::wstring env_block = BuildMergedEnvironmentBlock(extra_env, &env_error);
    if (!env_error.empty()) {
        result.spawn_failed = true;
        result.spawn_error = env_error;
        return result;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    // 一条读端句柄 + "是否已关"标记:读线程与收尾路径都要关它,谁先到谁关,
    // 另一头 CloseHandle(nullptr) 是空操作,不会双关。
    const auto make_pipe = [&](HANDLE& read_out, HANDLE& write_out) -> bool {
        return CreatePipe(&read_out, &write_out, &sa, 0) != FALSE;
    };

    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    HANDLE err_read = nullptr;
    HANDLE err_write = nullptr;
    if (!make_pipe(out_read, out_write) || !make_pipe(err_read, err_write)) {
        result.spawn_failed = true;
        result.spawn_error = "创建管道失败(错误码 " + std::to_string(GetLastError()) + ")";
        for (HANDLE h : {out_read, out_write, err_read, err_write}) {
            if (h != nullptr) {
                CloseHandle(h);
            }
        }
        return result;
    }
    // 父进程这边留着的读端不能被子进程继承,不然子进程退出后管道写端还有一份
    // 在父进程手里,ReadFile 会一直等不到 EOF。
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    // stdin:读端给子进程继承,写端留在父进程(不继承),写完就关。
    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        result.spawn_failed = true;
        result.spawn_error = "创建 stdin 管道失败(错误码 " + std::to_string(GetLastError()) + ")";
        for (HANDLE h : {out_read, out_write, err_read, err_write}) {
            CloseHandle(h);
        }
        return result;
    }
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out_write;
    si.hStdError = err_write;
    si.hStdInput = stdin_read;

    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back(L'\0');

    const BOOL ok = CreateProcessW(
        nullptr, cmdline_buf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
        const_cast<LPWSTR>(env_block.c_str()), nullptr, &si, &pi);

    // 子进程已经拿到了自己那份继承来的句柄,这里可以关了。
    CloseHandle(out_write);
    CloseHandle(err_write);
    CloseHandle(stdin_read);

    if (!ok) {
        CloseHandle(out_read);
        CloseHandle(err_read);
        CloseHandle(stdin_write);
        result.spawn_failed = true;
        result.spawn_error = "启动子进程失败(错误码 " + std::to_string(GetLastError()) + ")";
        return result;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    HANDLE overflow_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // stdin 写线程:一次性写完就关写端。子进程不读而数据超过管道缓冲时,
    // WriteFile 会阻塞到子进程死掉(读端关闭)为止——超时杀树正是一条
    // 解开它的路。
    std::atomic<bool> stdin_done{false};
    std::thread stdin_writer([&] {
        if (!stdin_data.empty()) {
            std::size_t written_total = 0;
            while (written_total < stdin_data.size()) {
                DWORD written = 0;
                const std::size_t chunk = std::min<std::size_t>(stdin_data.size() - written_total, 64 * 1024);
                if (!WriteFile(stdin_write, stdin_data.data() + written_total, static_cast<DWORD>(chunk), &written,
                               nullptr)) {
                    break;  // 子进程死了/管道断,EPIPE 类失败,写不完就写不完
                }
                written_total += written;
            }
        }
        CloseHandle(stdin_write);
        stdin_write = nullptr;
        stdin_done.store(true);
    });

    // 两条读线程:stdout 与 stderr 各自攒各自的原始字节。超限判定按两路
    // 合计——一路刷屏也算刷屏,照旧杀树。
    std::atomic<std::size_t> captured_total{0};
    std::atomic<bool> output_over_limit{false};
    const auto stream_reader = [&](HANDLE read_end, std::string* sink, std::atomic<bool>* done) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read_end, buf, sizeof(buf), &n, nullptr) && n > 0) {
            if (!output_over_limit.load()) {
                sink->append(buf, n);
                const std::size_t total = captured_total.fetch_add(n) + n;
                // off-by-one 对齐:total == limit 不算超限;读到第 limit+1 个
                // 字节(total > limit)才算。
                if (total > max_output_bytes) {
                    output_over_limit.store(true);
                    if (overflow_event != nullptr) {
                        SetEvent(overflow_event);
                    }
                }
            }
            // 超限之后继续读但直接丢弃——不读的话管道缓冲区一满,子进程会
            // 卡在写上死不掉;反正马上就要被杀,读空到 EOF 为止。
        }
        done->store(true);
    };
    std::string stdout_bytes;
    std::string stderr_bytes;
    std::atomic<bool> out_done{false};
    std::atomic<bool> err_done{false};
    std::thread out_reader([&] { stream_reader(out_read, &stdout_bytes, &out_done); });
    std::thread err_reader([&] { stream_reader(err_read, &stderr_bytes, &err_done); });

    const DWORD wait_ms = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : INFINITE;
    HANDLE wait_handles[2] = {pi.hProcess, overflow_event};
    const DWORD wait_count = overflow_event != nullptr ? 2 : 1;
    const DWORD wait_result = WaitForMultipleObjects(wait_count, wait_handles, FALSE, wait_ms);
    if (wait_result == WAIT_TIMEOUT || wait_result == WAIT_OBJECT_0 + 1) {
        if (wait_result == WAIT_TIMEOUT) {
            result.timed_out = true;
        }
        if (job != nullptr) {
            CloseHandle(job);  // KILL_ON_JOB_CLOSE:关句柄的一瞬间,job 里所有进程全杀
            job = nullptr;
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        WaitForSingleObject(pi.hProcess, 5000);
    }

    // 后代若继承了管道写端还活着,ReadFile 永远等不到 EOF——收掉 Job 连带
    // 杀光可能残留的后代,写端才会全关、读线程才能收尾。兜底(Job 创建/绑定
    // 失败过、杀不到后代):限时等读线程收尾,等不到就取消挂起的 ReadFile,
    // 绝不无限期阻塞。
    const auto join_reader = [](std::thread& reader, std::atomic<bool>& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        while (!done.load()) {
            CancelSynchronousIo(reader.native_handle());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        reader.join();
    };
    join_reader(out_reader, out_done);
    join_reader(err_reader, err_done);

    // stdin 写线程的收尾:子进程死后写端断裂,WriteFile 自然失败退出;万一
    // 还有谁挂着句柄不退,CancelSynchronousIo 取消挂起的写。
    const auto stdin_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!stdin_done.load() && std::chrono::steady_clock::now() < stdin_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!stdin_done.load()) {
        while (!stdin_done.load()) {
            CancelSynchronousIo(stdin_writer.native_handle());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    stdin_writer.join();
    if (stdin_write != nullptr) {
        CloseHandle(stdin_write);  // 写线程被取消时可能没走到关句柄那步
        stdin_write = nullptr;
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    result.exit_code = exit_code;
    // 两路各自对齐 UTF-8 边界(超限判定按两路合计,帽是合计帽:先合再对齐
    // 会把 stderr 的刀口错落在 stdout 尾上,所以各切各的)。
    AlignOutputToUtf8Boundary(stdout_bytes, max_output_bytes);
    AlignOutputToUtf8Boundary(stderr_bytes, max_output_bytes);
    result.output = stdout_bytes + stderr_bytes;  // 合并账(stdout 在前,保序可用)
    result.stdout_bytes = std::move(stdout_bytes);
    result.stderr_bytes = std::move(stderr_bytes);
    result.output_truncated = output_over_limit.load();

    if (overflow_event != nullptr) {
        CloseHandle(overflow_event);
    }
    CloseHandle(out_read);
    CloseHandle(err_read);
    CloseHandle(pi.hProcess);

    return result;
}

ProcessResult RunProcessWithStdin(const std::vector<std::string>& argv, const std::string& stdin_data,
                                  int timeout_ms, const EnvPairs& extra_env, std::size_t max_output_bytes) {
    ProcessResult result;
    if (argv.empty()) {
        result.spawn_failed = true;
        result.spawn_error = "argv 不能为空";
        return result;
    }
    return RunProcessWithStdinImpl(BuildProcessCommandLine(argv[0], std::vector<std::string>(argv.begin() + 1, argv.end())),
                                   stdin_data, timeout_ms, extra_env, max_output_bytes);
}

ProcessResult RunShellCommandWithStdin(const std::string& command_utf8, const std::string& stdin_data,
                                       int timeout_ms, const EnvPairs& extra_env, std::size_t max_output_bytes) {
    // 不再做整段 AcpBytesToUtf8:stdout/stderr 已分开捕获为原始字节,cmd.exe
    // 的 ANSI 输出由 hooks 解码层"先认 UTF-8、次选明示代码页"处理,命中哪
    // 一档都有标注,不无声替换。
    return RunProcessWithStdinImpl(BuildCmdCommandLine(command_utf8), stdin_data, timeout_ms, extra_env,
                                   max_output_bytes);
}

// ---------------------------------------------------------------------------
// 后台模式:spawn 立刻返回,不等待、不捕获进内存,stdout/stderr 直接指到
// 日志文件的句柄上(CreateProcessW 层面重定向,不经过管道/读线程)。
// ---------------------------------------------------------------------------

BackgroundSpawnResult RunProcessBackground(const std::wstring& cmdline, const std::string& cwd_utf8,
                                            const EnvPairs& extra_env) {
    BackgroundSpawnResult result;

    // 显式环境块(P0 并发修复):不再临时改宿主环境。Hook dispatcher 给每只
    // handler 一条线程,老路(改父环境→起子进程→还原)并发时会串值。
    std::string env_error;
    const std::wstring env_block = BuildMergedEnvironmentBlock(extra_env, &env_error);
    if (!env_error.empty()) {
        result.error = env_error;
        return result;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    const std::wstring log_path_w = BuildBackgroundLogPathW();
    // FILE_SHARE_READ:日志还在写的时候,模型那边"用普通命令看日志"
    // (Get-Content 之类)要能同时打开读,不能被这里的写句柄独占锁死。
    // CREATE_NEW(独占):文件名虽带时间戳+计数器,共享临时目录里被人预置
    // 同名/reparse point 时不沿用别人的文件;撞名换一个再来。
    HANDLE log_file = CreateFileW(log_path_w.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    std::wstring log_path_actual = log_path_w;
    if (log_file == INVALID_HANDLE_VALUE) {
        log_file = CreateFileW(BuildBackgroundLogPathW().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        log_path_actual = BuildBackgroundLogPathW();
    }
    if (log_file == INVALID_HANDLE_VALUE) {
        result.error = "创建日志文件失败(错误码 " + std::to_string(GetLastError()) + ")";
        return result;
    }

    HANDLE stdin_null = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    // 句柄白名单(P1):bInheritHandles=TRUE 会让当时所有可继承句柄都有机会
    // 落进子进程;并发起进程时别处刚建的 pipe/file 可能被误继承。用
    // STARTUPINFOEX + PROC_THREAD_ATTRIBUTE_HANDLE_LIST 只传 stdin/stdout/
    // stderr 三只指定句柄。
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = log_file;
    si.StartupInfo.hStdError = log_file;
    si.StartupInfo.hStdInput = stdin_null;

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    if (!InitializeProcThreadAttributeList(reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data()), 1, 0,
                                           &attr_size)) {
        CloseHandle(log_file);
        if (stdin_null != nullptr && stdin_null != INVALID_HANDLE_VALUE) {
            CloseHandle(stdin_null);
        }
        result.error = "初始化进程属性表失败(错误码 " + std::to_string(GetLastError()) + ")";
        return result;
    }
    HANDLE inherit_list[3] = {log_file, log_file, stdin_null};
    const bool attr_ok = UpdateProcThreadAttribute(
        reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data()), 0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit_list, sizeof(inherit_list), nullptr, nullptr) != FALSE;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back(L'\0');

    // cwd 走 lpCurrentDirectory(P1 根治):不再向命令文本前拼 cd。
    std::wstring cwd_wide;
    LPCWSTR current_directory = nullptr;
    if (!cwd_utf8.empty()) {
        cwd_wide = Utf8ToWide(cwd_utf8);
        current_directory = cwd_wide.c_str();
    }

    BOOL ok = FALSE;
    DWORD last_error = 0;
    if (attr_ok) {
        ok = CreateProcessW(nullptr, cmdline_buf.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                            const_cast<LPWSTR>(env_block.c_str()), current_directory,
                            &si.StartupInfo, &pi);
        last_error = GetLastError();
    } else {
        // 属性表更新失败(老系统):退化成普通继承,照常起(降级,不硬失败)。
        ok = CreateProcessW(nullptr, cmdline_buf.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                            const_cast<LPWSTR>(env_block.c_str()), current_directory, &si.StartupInfo, &pi);
        last_error = GetLastError();
    }
    DeleteProcThreadAttributeList(reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data()));
    CloseHandle(log_file);
    if (stdin_null != nullptr && stdin_null != INVALID_HANDLE_VALUE) {
        CloseHandle(stdin_null);
    }

    if (!ok) {
        result.error = "启动子进程失败(错误码 " + std::to_string(last_error) + ")";
        return result;
    }

    // 每任务专属 Job(P0):Stop 落 TerminateJobObject 收整棵树(npm run dev
    // 的后代一起死)。会话级 Job 仍然挂一道(宿主退出兜底);进程可以同时
    // 属于嵌套 Job(Vista+ 语义,Windows 8+ 默认允许)。绑不上专属 Job 时
    // 如实降级——Stop 只能杀根,handle 上的树级杀不动。
    auto handle = std::make_shared<BackgroundProcessHandle>();
    handle->impl->process = pi.hProcess;
    HANDLE task_job = CreateJobObjectW(nullptr, nullptr);
    bool job_ok = false;
    if (task_job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(task_job, JobObjectExtendedLimitInformation, &limit, sizeof(limit))) {
            job_ok = AssignProcessToJobObject(task_job, pi.hProcess) != FALSE;
        }
        if (!job_ok) {
            CloseHandle(task_job);
            task_job = nullptr;
        }
    }
    handle->impl->job = task_job;
    // 会话级兜底 job:宿主退出时收掉漏网的。失败不致命(顶多这条兜底没有)。
    AssignProcessToJobObject(GetBackgroundSessionJob(), pi.hProcess);
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        // resume 失败(P1 全量验错):进程吊在 suspended,杀掉、关句柄、回错。
        if (task_job != nullptr) {
            TerminateJobObject(task_job, 1);
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        WaitForSingleObject(pi.hProcess, 3000);
        result.error = "恢复子进程线程失败(错误码 " + std::to_string(GetLastError()) + ")";
        return result;
    }
    CloseHandle(pi.hThread);

    result.success = true;
    result.pid = pi.dwProcessId;
    result.log_path = WideToUtf8(log_path_actual);
    result.handle = std::move(handle);
    return result;
}

BackgroundSpawnResult RunProcessBackground(const std::vector<std::string>& argv, const EnvPairs& extra_env) {
    return RunProcessBackground(argv, std::string(), extra_env);
}

BackgroundSpawnResult RunProcessBackground(const std::wstring& cmdline, const EnvPairs& extra_env) {
    return RunProcessBackground(cmdline, std::string(), extra_env);
}

BackgroundSpawnResult RunShellCommandBackground(const std::string& command_utf8, const EnvPairs& extra_env) {
    return RunShellCommandBackground(command_utf8, std::string(), extra_env);
}

BackgroundSpawnResult RunProcessBackground(const std::vector<std::string>& argv, const std::string& cwd_utf8,
                                            const EnvPairs& extra_env) {
    if (argv.empty()) {
        BackgroundSpawnResult result;
        result.error = "argv 不能为空";
        return result;
    }
    return RunProcessBackground(
        BuildProcessCommandLine(argv[0], std::vector<std::string>(argv.begin() + 1, argv.end())), cwd_utf8,
        extra_env);
}

BackgroundSpawnResult RunShellCommandBackground(const std::string& command_utf8, const std::string& cwd_utf8,
                                                 const EnvPairs& extra_env) {
    BackgroundSpawnResult result = RunProcessBackground(BuildCmdCommandLine(command_utf8), cwd_utf8, extra_env);
    if (result.success) {
        // cmd.exe 落盘的是 OEM/ACP 字节,background_output 出口按这个洗。
        result.handle->encoding_hint = "oem-ansi";
    }
    return result;
}

// ---------------------------------------------------------------------------
// ChildProcess:长命双向管道(搬自 mcp/transport.cpp 的 StdioTransport)。
// ---------------------------------------------------------------------------

ChildProcess::~ChildProcess() {
    Shutdown(2000);
}

SpawnResult ChildProcess::Start(const std::string& command, const std::vector<std::string>& args,
                                  const EnvPairs& env, std::function<bool(std::string_view)> on_stdout,
                                  std::function<void(std::string_view)> on_stderr, const std::string& cwd_utf8,
                                  EnvMode env_mode) {
    return Start(command, args, env, std::move(on_stdout), std::move(on_stderr), SpawnConstraints{}, cwd_utf8,
                 env_mode);
}

SpawnResult ChildProcess::Start(const std::string& command, const std::vector<std::string>& args,
                                  const EnvPairs& env, std::function<bool(std::string_view)> on_stdout,
                                  std::function<void(std::string_view)> on_stderr,
                                  const SpawnConstraints& constraints, const std::string& cwd_utf8,
                                  EnvMode env_mode) {
    on_stdout_ = std::move(on_stdout);
    on_stderr_ = std::move(on_stderr);

    // 环境块两种模式都走 lpEnvironment,不再动宿主环境变量(P0 并发修复:
    // MCP/LSP 起多条长命子进程与 Hook 并发 spawn 同住一个宿主,老路会串值):
    //   Replace:只含 env 条目(plugins 单第 8 步的最小环境硬保证);
    //   Inherit:父环境快照 + env 覆盖(BuildMergedEnvironmentBlock)。
    const bool replace_env = env_mode == EnvMode::Replace;
    std::wstring explicit_env;
    if (replace_env) {
        explicit_env = BuildExplicitEnvironment(env);
    } else {
        std::string env_error;
        explicit_env = BuildMergedEnvironmentBlock(env, &env_error);
        if (!env_error.empty()) {
            return SpawnResult{false, env_error, false};
        }
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE stdin_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_write = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;
    HANDLE stderr_read = nullptr;

    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        return SpawnResult{false, "创建 stdin 管道失败(错误码 " + std::to_string(GetLastError()) + ")", false};
    }
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
        CloseHandle(stdin_read);
        CloseHandle(stdin_write);
        return SpawnResult{false, "创建 stdout 管道失败(错误码 " + std::to_string(GetLastError()) + ")", false};
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
        CloseHandle(stdin_read);
        CloseHandle(stdin_write);
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        return SpawnResult{false, "创建 stderr 管道失败(错误码 " + std::to_string(GetLastError()) + ")", false};
    }
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;

    PROCESS_INFORMATION pi{};
    std::wstring cmdline = BuildProcessCommandLine(command, args);
    std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back(L'\0');

    // PTC 沙箱:受限 token(自己 token 的禁权版,CreateProcessAsUser 允许
    // 无特权进程用"自己派生的受限 token"起子进程)。造不出就照常起,调用
    // 方的沙箱档位自己降级记档。
    HANDLE child_token = nullptr;
    if (constraints.restricted_token) {
        HANDLE current = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &current)) {
            CreateRestrictedToken(current, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr, &child_token);
            CloseHandle(current);
        }
    }

    // cwd:非空则子进程的工作目录指过去(UTF-8 转宽;空 = 继承本进程)。
    std::wstring cwd_wide;
    LPCWSTR current_directory = nullptr;
    if (!cwd_utf8.empty()) {
        cwd_wide = Utf8ToWide(cwd_utf8);
        current_directory = cwd_wide.c_str();
    }

    // lpEnvironment:两种模式都交显式块(CREATE_UNICODE_ENVIRONMENT)。
    LPVOID environment = const_cast<wchar_t*>(explicit_env.c_str());
    DWORD creation_flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;

    BOOL ok = FALSE;
    DWORD error_code = 0;
    if (child_token != nullptr) {
        ok = CreateProcessAsUserW(child_token, nullptr, cmdline_buf.data(), nullptr, nullptr, TRUE,
                                  creation_flags, environment, current_directory, &si, &pi);
        CloseHandle(child_token);
        if (!ok) {
            // 受限 token 这条路走不通(策略/权限),降级回普通创建,不硬失败。
            ok = CreateProcessW(nullptr, cmdline_buf.data(), nullptr, nullptr, TRUE,
                                creation_flags, environment, current_directory, &si, &pi);
        }
    } else {
        ok = CreateProcessW(nullptr, cmdline_buf.data(), nullptr, nullptr, TRUE,
                            creation_flags, environment, current_directory, &si, &pi);
    }
    error_code = GetLastError();

    // 子进程该拿到的句柄已经拿到了(继承来的),父进程这边的可以关了。
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);

    if (!ok) {
        CloseHandle(stdin_write);
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        SpawnResult spawn{};
        spawn.success = false;
        spawn.error = "启动子进程失败(错误码 " + std::to_string(error_code) + "): " + command;
        spawn.command_not_found = (error_code == ERROR_FILE_NOT_FOUND || error_code == ERROR_PATH_NOT_FOUND);
        return spawn;
    }

    process_ = pi.hProcess;
    stdin_write_ = stdin_write;
    stdout_read_ = stdout_read;
    stderr_read_ = stderr_read;

    // Job Object:超时/主动关停时,关掉 job 句柄能把整棵进程树一起杀掉。
    // PTC 沙箱约束在这一并落墙:CPU 时间(PROCESS_TIME,100ns 单位)与
    // 进程内存(PROCESS_MEMORY)。设不上只降级,照常跑。
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (constraints.cpu_seconds > 0) {
            limit.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_TIME;
            limit.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
                static_cast<LONGLONG>(constraints.cpu_seconds) * 10000000LL;
        }
        if (constraints.memory_bytes > 0) {
            limit.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            limit.ProcessMemoryLimit = constraints.memory_bytes;
        }
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    job_ = job;
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    started_ = true;
    stdout_thread_ = std::thread([this] { StdoutReaderThread(); });
    stderr_thread_ = std::thread([this] { StderrReaderThread(); });

    return SpawnResult{true, std::string(), false};
}

void ChildProcess::StdoutReaderThread() {
    char buf[4096];
    DWORD n = 0;
    while (!reader_stop_.load() && ReadFile(static_cast<HANDLE>(stdout_read_), buf, sizeof(buf), &n, nullptr) &&
           n > 0) {
        if (on_stdout_ && !on_stdout_(std::string_view(buf, n))) {
            break;  // 回调宣布这条流报废(MCP 单行超限断连那条路),不再读
        }
    }
    stdout_reader_done_.store(true);
}

void ChildProcess::StderrReaderThread() {
    char buf[4096];
    DWORD n = 0;
    while (!reader_stop_.load() && ReadFile(static_cast<HANDLE>(stderr_read_), buf, sizeof(buf), &n, nullptr) &&
           n > 0) {
        if (on_stderr_) {
            on_stderr_(std::string_view(buf, n));
        }
    }
    stderr_reader_done_.store(true);
}

void ChildProcess::JoinReaderThreads() {
    // 兜底:进程树都该死透了,正常情况下写端全关、ReadFile 返回 0,线程
    // 自己收尾。万一有后代进程漏杀(没有 Job)还握着写端,ReadFile 会一直
    // 挂着——限时等一等,等不到就置停止标志 + 取消挂起的同步 IO,绝不吊死。
    const auto wait_reader = [this](std::thread& thread, std::atomic<bool>& done) {
        if (!thread.joinable()) {
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!done.load()) {
            reader_stop_.store(true);
            while (!done.load()) {
                CancelSynchronousIo(thread.native_handle());
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        thread.join();
    };
    wait_reader(stdout_thread_, stdout_reader_done_);
    wait_reader(stderr_thread_, stderr_reader_done_);
}

bool ChildProcess::Write(const std::string& data) {
    if (!started_ || stdin_write_ == nullptr || !IsAlive()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(write_mutex_);
    DWORD written = 0;
    std::size_t offset = 0;
    while (offset < data.size()) {
        if (!WriteFile(static_cast<HANDLE>(stdin_write_), data.data() + offset,
                        static_cast<DWORD>(data.size() - offset), &written, nullptr)) {
            return false;
        }
        if (written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

void ChildProcess::Kill() {
    if (process_ != nullptr) {
        TerminateProcess(static_cast<HANDLE>(process_), 1);
    }
}

void ChildProcess::CloseStdin() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (stdin_write_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(stdin_write_));
        stdin_write_ = nullptr;
    }
}

void ChildProcess::Shutdown(int wait_ms) {
    if (!started_ || shutdown_done_.exchange(true)) {
        return;
    }

    // 先关 stdin——关掉写端相当于给子进程发了个 EOF,行为良好的服务器看到
    // stdin EOF 会自己体面退出(协议层要发的 shutdown 请求在这之前已经发过)。
    if (stdin_write_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(stdin_write_));
        stdin_write_ = nullptr;
    }

    if (process_ != nullptr) {
        const DWORD wait_result =
            WaitForSingleObject(static_cast<HANDLE>(process_), wait_ms > 0 ? static_cast<DWORD>(wait_ms) : 0);
        if (wait_result != WAIT_OBJECT_0) {
            // 还没退出:Job Object 连带子子进程一起杀掉;没有 job(创建/绑定
            // 失败过)就退化成只杀主进程。
            if (job_ != nullptr) {
                CloseHandle(static_cast<HANDLE>(job_));
                job_ = nullptr;
            } else {
                TerminateProcess(static_cast<HANDLE>(process_), 1);
            }
            WaitForSingleObject(static_cast<HANDLE>(process_), 5000);
        }
    }

    // PTC 沙箱:句柄关掉前把资源账与退出码读出来缓存(撞线归因用)。
    if (job_ != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        if (QueryInformationJobObject(static_cast<HANDLE>(job_), JobObjectExtendedLimitInformation, &info,
                                      sizeof(info), nullptr)) {
            resource_usage_cache_.peak_memory_bytes = info.PeakProcessMemoryUsed;
        }
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (QueryInformationJobObject(static_cast<HANDLE>(job_), JobObjectBasicAccountingInformation, &accounting,
                                      sizeof(accounting), nullptr)) {
            // TotalUserTime 是 job 内全部进程的用户态 CPU 合计(100ns)。
            resource_usage_cache_.cpu_100ns = accounting.TotalUserTime.QuadPart;
        }
    }
    if (process_ != nullptr) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(static_cast<HANDLE>(process_), &exit_code)) {
            exit_code_cache_ = static_cast<int>(exit_code);
        }
    }

    // 根进程退了不代表后代也退了:后代若继承了 stdout/stderr 写端还活着,
    // 读线程等不到 EOF。先把 Job 收掉(KILL_ON_JOB_CLOSE 连带清光后代),
    // 写端才会全关,读线程才能正常收尾;JoinReaderThreads 里另有限时+取消
    // IO 的兜底。
    if (job_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(job_));
        job_ = nullptr;
    }
    JoinReaderThreads();

    if (stdout_read_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(stdout_read_));
        stdout_read_ = nullptr;
    }
    if (stderr_read_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(stderr_read_));
        stderr_read_ = nullptr;
    }
    if (process_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(process_));
        process_ = nullptr;
    }
}

ChildResourceUsage ChildProcess::ResourceUsageSnapshot() const { return resource_usage_cache_; }

int ChildProcess::exit_code() const { return exit_code_cache_; }

bool ChildProcess::IsAlive() const {
    if (process_ == nullptr) {
        return false;
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(process_), &exit_code)) {
        return false;
    }
    return exit_code == STILL_ACTIVE;
}

bool IsProcessAlive(unsigned long pid) {
    if (pid == 0) {
        return false;
    }
    if (pid == GetCurrentProcessId()) {
        return true;
    }
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        // 打不开可能是权限不够(别的会话/提权进程),按"活着"算,交给心跳
        // 过期那条路,不误删。
        return true;
    }
    DWORD exit_code = 0;
    const bool got = GetExitCodeProcess(process, &exit_code) != FALSE;
    CloseHandle(process);
    return got && exit_code == STILL_ACTIVE;
}

unsigned long CurrentProcessId() { return static_cast<unsigned long>(GetCurrentProcessId()); }

int RunInteractiveCommand(const std::string& command_utf8) {
    // _wsystem:继承当前控制台、等子进程跑完。编辑器(vim/notepad)自己
    // 管自己的控制台模式,不需要我们喂 stdin/收 stdout。
    if (command_utf8.empty()) {
        return -1;
    }
    const std::wstring wide = Utf8ToWide(command_utf8);
    return _wsystem(wide.c_str());
}

}  // namespace lubancode::platform
