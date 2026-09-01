// SearchTool 内置 ripgrep 后端迁移单 P0-7:性能与稳定门驱动(手动跑,不进
// ctest——要真 rg、要长跑)。
//
// 子命令:
//   bench   <rg路径> <输出JSON> [--small-dir D] [--small-file F] [--medium D]
//           [--medium-p01 D] [--large D] [--rounds 7] [--csv PATH]
//       三档以上语料 x 五类查询 x 两列(direct rg / SearchTool+rg) x N 轮:
//       wall time 首轮(冷)与余轮(热)分开、P50/P95、rg 子进程峰值内存与
//       CPU、宿主进程峰值内存/句柄/线程。旧内核列不重跑(内核已删),由
//       report 子命令按 query id 对 P0-1 冻结的
//       tests/fixtures/search/bench/old_kernel_bench_src.json 对账。
//   stress  <rg路径> <输出JSON> --corpus D [--rounds 1000]
//           [--large D] [--large-rounds 100] [--round-cap-ms N]
//       稳定门:短搜索压力 N 轮(默认 1000,单子 P0-7 首版性能门)+ 大树
//       M 轮(默认 100;Linux 侧即设计单 §3.2 的 15.2.0 资产压力门)。逐轮
//       看板挂死,收尾查孤儿 rg、宿主句柄/线程账。任一轮挂死/崩溃/留孤儿
//       即 exit 1。
//   cancel  <rg路径> <输出JSON> --corpus D [--delay-ms 200] [--rounds 10]
//       取消响应延迟:大树无命中长查跑到半路按停,量"置取消旗 -> execute
//       返回"与"置取消旗 -> 进程表无 rg 残留"两段耗时;另带起跑前取消与
//       不取消对照轮。
//   report  <旧内核bench.json> <新raw.json...> --out <报告JSON> [--csv PATH]
//       对账与门判:旧内核 P50/P95 vs 新后端 P50/P95(同语料同查询 id),
//       量提速倍数;SearchTool+rg vs direct rg 的包装开销过门判定(超
//       max(20%, 20ms) 记红);大树高频命中截断的亚线性证据。数字只从
//       JSON 来,不预写结论。
//
// 内存采集方法(如实记录,交 P0-1 留白的账):
//   - rg 子进程:采样线程每 5ms 扫进程表(Windows: Toolhelp32 快照 +
//     K32GetProcessMemoryInfo 取 WorkingSet/PeakWorkingSet、GetProcessTimes
//     取 CPU;POSIX: /proc/<pid>/status 的 VmRSS/VmHWM、stat 的 utime+stime),
//     按可执行名 == rg 名过滤。PeakWorkingSetSize/VmHWM 是进程生命周期峰,
//     采到一拍即真。P0-1 的 psapi.h 编译冲突在本文件以"独立文件 +
//     NOMINMAX/WIN32_LEAN_AND_MEAN + windows.h 后置"绕开,生产代码零改动。
//   - direct rg 列在 Windows 另有精确账:驱动自持 ChildProcess,Shutdown 后
//     ResourceUsageSnapshot() 直接给 Job Object 的 PeakProcessMemoryUsed 与
//     TotalUserTime,与采样账互为印证。
//   - 宿主进程:自采(Windows K32GetProcessMemoryInfo+GetProcessHandleCount;
//     POSIX /proc/self/status 的 VmHWM/Threads 与 /proc/self/fd 计数)。
//
// 基准口径(与 P0-1 旧 bench 对齐,保证可比):
//   - 轮内首轮当"冷"记,余轮当"热"记(同 P0-1 的穷人版冷热分账;OS 页缓存
//     由语料生成过程预热,本驱动测的是热机数,如实标注);
//   - P50/P95 用全部轮样本(同 P0-1 的 Percentile 口径);
//   - 文件数用同一套跳过规则(.git/build/node_modules/.evidence)独立数一遍。

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/process.hpp"
#include "tools/path_utils.hpp"
#include "tools/search.hpp"
#include "tools/search_ripgrep.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
// P0-1 留白笔账:psapi.h 直含在本仓旧文件里曾与既有 include 序撞车;这里
// 独立文件 + 宏防护 + windows.h 先行,直含可用(编译即证)。PSAPI_VERSION=2
// 把 GetProcessMemoryInfo 映射到 kernel32 的 K32 变体,MSVC/MinGW 都认。
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <psapi.h>
#endif

using lubancode::platform::ChildProcess;
using lubancode::platform::SpawnResult;
using lubancode::tools::BuildGlobArgv;
using lubancode::tools::BuildGrepArgv;
using lubancode::tools::BundledRipgrepRunner;
using lubancode::tools::PathToUtf8;
using lubancode::tools::SearchRequest;
using lubancode::tools::SearchTool;
using lubancode::tools::ToString;
using lubancode::tools::Tool;
using lubancode::tools::Utf8ToPath;

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

double ElapsedMs(Clock::time_point t0, Clock::time_point t1) {
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

double Percentile(std::vector<double> samples, double pct) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const double max_idx = static_cast<double>(samples.size() - 1);
    const std::size_t idx =
        static_cast<std::size_t>(std::min<double>(max_idx, pct / 100.0 * max_idx));
    return samples[idx];
}

// ---- 进程表监视(两平台同名采样线程) ---------------------------------------

struct ProcSample {
    std::uint64_t current_ws = 0;
    std::uint64_t peak_ws = 0;      // 进程生命周期峰(单拍即真)
    std::uint64_t cpu_ms = 0;       // user+kernel 累计
    std::size_t procs_seen = 0;     // 本拍匹配的进程数
};

struct HostSnapshot {
    std::uint64_t peak_ws = 0;
    std::uint64_t handles = 0;      // Windows: 句柄数;POSIX: 打开 fd 数
    std::uint64_t threads = 0;
};

// 进程归属:这台开发机上常有别的会话(其他 agent/编辑器)也在跑 rg,那是
// 正当外来户。我们起的 rg 必是本驱动的直接子进程(runner 的 ChildProcess
// 在本进程内起 rg;direct 列亦然),按"exe 名匹配 && 父进程 == 本进程"
// 归属,外来户只计数不拦路。
class ProcessMonitor {
public:
    explicit ProcessMonitor(std::string rg_exe_name)
        : rg_name_(std::move(rg_exe_name)) {}

    // 本驱动名下的 rg 进程数(孤儿/残留检测用)。
    static std::size_t CountOurRg(const std::string& rg_name);
    // 名字匹配但父进程不是本驱动的 rg 数(诊断记账,不影响判门)。
    static std::size_t CountForeignRg(const std::string& rg_name);

    // 开一个"采样窗口":窗口内盯住本驱动名下的 rg,Stop 时交峰值账。
    void StartWindow();
    ProcSample StopWindow();

    static HostSnapshot Host();

private:
    void SamplerLoop();
    static ProcSample SampleOnce(const std::string& rg_name, bool ours_only);

    std::string rg_name_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    ProcSample window_;      // StartWindow 清零,SamplerLoop 累计
    std::size_t samples_ = 0;
};

#ifdef _WIN32

ProcSample ProcessMonitor::SampleOnce(const std::string& rg_name, bool ours_only) {
    ProcSample out;
    // 名字统一小写比(NTFS 大小写不敏感)。
    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    };
    const std::string want = lower(rg_name);
    const DWORD self_pid = ::GetCurrentProcessId();
    const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snap, &entry)) {
        do {
            std::wstring wide(entry.szExeFile);
            const std::string name =
                lower(std::string(wide.begin(), wide.end()));
            if (name != want) continue;
            if (ours_only && entry.th32ParentProcessID != self_pid) continue;
            const HANDLE proc =
                ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (proc == nullptr) continue;
            PROCESS_MEMORY_COUNTERS pmc{};
            pmc.cb = sizeof(pmc);
            if (::GetProcessMemoryInfo(proc, &pmc, sizeof(pmc))) {
                out.current_ws = std::max<std::uint64_t>(out.current_ws, pmc.WorkingSetSize);
                out.peak_ws = std::max<std::uint64_t>(out.peak_ws, pmc.PeakWorkingSetSize);
            }
            FILETIME create{}, exit{}, kernel{}, user{};
            if (::GetProcessTimes(proc, &create, &exit, &kernel, &user)) {
                const auto ticks = [](const FILETIME& t) -> std::uint64_t {
                    ULARGE_INTEGER u{};
                    u.LowPart = t.dwLowDateTime;
                    u.HighPart = t.dwHighDateTime;
                    return u.QuadPart;  // 100ns
                };
                out.cpu_ms = std::max<std::uint64_t>(out.cpu_ms,
                                                     (ticks(kernel) + ticks(user)) / 10000);
            }
            ++out.procs_seen;
            ::CloseHandle(proc);
        } while (::Process32NextW(snap, &entry));
    }
    ::CloseHandle(snap);
    return out;
}

std::size_t ProcessMonitor::CountOurRg(const std::string& rg_name) {
    return SampleOnce(rg_name, /*ours_only=*/true).procs_seen;
}

std::size_t ProcessMonitor::CountForeignRg(const std::string& rg_name) {
    const std::size_t all = SampleOnce(rg_name, /*ours_only=*/false).procs_seen;
    return all - SampleOnce(rg_name, /*ours_only=*/true).procs_seen;
}

HostSnapshot ProcessMonitor::Host() {
    HostSnapshot out;
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc))) {
        out.peak_ws = pmc.PeakWorkingSetSize;
    }
    DWORD handles = 0;
    if (::GetProcessHandleCount(::GetCurrentProcess(), &handles)) {
        out.handles = handles;
    }
    const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        const DWORD self_pid = ::GetCurrentProcessId();
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (::Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID == self_pid) ++out.threads;
            } while (::Thread32Next(snap, &te));
        }
        ::CloseHandle(snap);
    }
    return out;
}

#else  // POSIX

#include <unistd.h>

ProcSample ProcessMonitor::SampleOnce(const std::string& rg_name, bool ours_only) {
    ProcSample out;
    std::error_code ec;
    const long self_pid = ::getpid();
    for (const auto& pid_dir : fs::directory_iterator("/proc", ec)) {
        const std::string pid_name = pid_dir.path().filename().string();
        if (pid_name.empty() ||
            !std::all_of(pid_name.begin(), pid_name.end(),
                         [](unsigned char c) { return std::isdigit(c) != 0; })) {
            continue;
        }
        std::error_code link_ec;
        const fs::path exe = fs::read_symlink(pid_dir.path() / "exe", link_ec);
        if (link_ec || exe.filename().string() != rg_name) continue;
        if (ours_only) {
            // /proc/<pid>/stat 字段 4 是 ppid;comm 可含空格,从 ')' 后倒数。
            long ppid = -1;
            std::ifstream stat(pid_dir.path() / "stat");
            std::string line;
            std::getline(stat, line);
            const std::size_t close = line.rfind(')');
            if (close != std::string::npos) {
                std::istringstream rest(line.substr(close + 2));
                std::string state;
                long p = 0;
                if (rest >> state >> p) ppid = p;
            }
            if (ppid != self_pid) continue;
        }
        std::uint64_t rss = 0, hwm = 0;
        {
            std::ifstream status(pid_dir.path() / "status");
            std::string key, unit;
            unsigned long long val = 0;
            while (status >> key) {
                if (key == "VmRSS:" || key == "VmHWM:") {
                    status >> val >> unit;
                    if (key == "VmRSS:") rss = val * 1024;
                    else hwm = val * 1024;
                } else {
                    std::string rest;
                    std::getline(status, rest);
                }
            }
        }
        std::uint64_t cpu_ms = 0;
        {
            std::ifstream stat(pid_dir.path() / "stat");
            std::string line;
            std::getline(stat, line);
            // 字段 14/15 是 utime/stime(单位 clock tick,通常 100Hz);comm
            // 字段可能带空格括号,从末尾倒数更稳:state.. 后 11 个字段即 14。
            const std::size_t close = line.rfind(')');
            if (close != std::string::npos) {
                std::istringstream rest(line.substr(close + 2));
                std::vector<std::string> fields;
                std::string f;
                while (rest >> f) fields.push_back(f);
                if (fields.size() >= 13) {
                    const std::uint64_t utime = std::stoull(fields[11]);
                    const std::uint64_t stime = std::stoull(fields[12]);
                    const long hz = ::sysconf(_SC_CLK_TCK);
                    cpu_ms = (utime + stime) * 1000 / (hz > 0 ? hz : 100);
                }
            }
        }
        out.current_ws = std::max(out.current_ws, rss);
        out.peak_ws = std::max(out.peak_ws, hwm);
        out.cpu_ms = std::max(out.cpu_ms, cpu_ms);
        ++out.procs_seen;
    }
    return out;
}

std::size_t ProcessMonitor::CountOurRg(const std::string& rg_name) {
    return SampleOnce(rg_name, /*ours_only=*/true).procs_seen;
}

std::size_t ProcessMonitor::CountForeignRg(const std::string& rg_name) {
    const std::size_t all = SampleOnce(rg_name, /*ours_only=*/false).procs_seen;
    return all - SampleOnce(rg_name, /*ours_only=*/true).procs_seen;
}

HostSnapshot ProcessMonitor::Host() {
    HostSnapshot out;
    {
        std::ifstream status("/proc/self/status");
        std::string key, unit;
        unsigned long long val = 0;
        while (status >> key) {
            if (key == "VmHWM:" || key == "Threads:") {
                status >> val >> unit;
                if (key == "VmHWM:") out.peak_ws = val * 1024;
                else out.threads = val;
            } else {
                std::string rest;
                std::getline(status, rest);
            }
        }
    }
    std::error_code ec;
    out.handles = static_cast<std::uint64_t>(
        std::distance(fs::directory_iterator("/proc/self/fd", ec),
                      fs::directory_iterator()));
    return out;
}

#endif

void ProcessMonitor::SamplerLoop() {
    while (!stop_.load(std::memory_order_relaxed)) {
        ProcSample s = SampleOnce(rg_name_, /*ours_only=*/true);
        if (s.procs_seen > 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            window_.peak_ws = std::max(window_.peak_ws, s.peak_ws);
            window_.current_ws = std::max(window_.current_ws, s.current_ws);
            window_.cpu_ms = std::max(window_.cpu_ms, s.cpu_ms);
        }
        ++samples_;
        // 2ms:短命 rg(小档几十毫秒)也要在生长期多采几拍;PeakWorkingSet
        // 是生命周期峰,越晚一拍越真。
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void ProcessMonitor::StartWindow() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        window_ = ProcSample{};
        samples_ = 0;
    }
    stop_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { SamplerLoop(); });
}

ProcSample ProcessMonitor::StopWindow() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    return window_;
}

// ---- 查询表(每档一套;id 与 P0-1 旧 bench 对齐) ----------------------------

struct BenchQuery {
    std::string id;
    std::string mode;   // "grep" | "glob"
    std::string pattern;
    std::string glob;   // 仅 grep
    std::string note;
};

std::vector<BenchQuery> OldBenchAlignedQueries() {
    // 与 tests/fixtures/search/bench/old_kernel_bench_src.json 逐字对齐的
    // 五类(medium 档用这套,report 按 id 对账)。
    return {
        {"literal_common_word", "grep", "SearchTool", "", "字面量,若干处命中"},
        {"regex_moderate", "grep", "std::[A-Za-z_]+<", "", "普通正则(模板类型)"},
        {"no_match", "grep", "definitely_absent_zzz_token_12345", "", "全仓无命中"},
        {"high_frequency", "grep", "the", "", "高频命中,触发 100 条截断"},
        {"glob_enum_cpp", "glob", "*.cpp", "", "glob 枚举 .cpp"},
    };
}

std::vector<BenchQuery> LargeCorpusQueries() {
    // scripts/gen_search_bench_corpus.py 埋的探针(见 corpus_manifest.json)。
    return {
        {"literal_common_word", "grep", "bench_needle_common", "", "字面量,~30% 文件,截断"},
        {"regex_moderate", "grep", "std::[A-Za-z_]+<", "", "普通正则,截断"},
        {"no_match", "grep", "definitely_absent_zzz_token_12345", "", "全树无命中(全文扫描)"},
        {"high_frequency", "grep", "the", "", "高频命中,触发 100 条截断"},
        {"glob_enum_cpp", "glob", "*.cpp", "", "glob 枚举 .cpp(截断)"},
    };
}

std::vector<BenchQuery> SmallDirQueries() {
    // tests/fixtures/search/corpus 这批小夹具的针。
    return {
        {"literal_common_word", "grep", "needle", "", "字面量(夹具针)"},
        {"regex_moderate", "grep", "nee[a-z]+_[a-z]+", "", "普通正则"},
        {"no_match", "grep", "definitely_absent_zzz_token_12345", "", "无命中"},
        {"high_frequency", "grep", "e", "", "高频命中,截断"},
        {"glob_enum_txt", "glob", "*.txt", "", "glob 枚举 .txt"},
    };
}

std::vector<BenchQuery> SmallFileQueries() {
    return {
        {"literal_common_word", "grep", "bench_needle_common", "", "单文件字面量"},
        {"regex_moderate", "grep", "std::[A-Za-z_]+<", "", "单文件正则"},
        {"no_match", "grep", "definitely_absent_zzz_token_12345", "", "单文件无命中"},
        {"high_frequency", "grep", "the", "", "单文件高频(截断)"},
        {"glob_enum_cpp", "glob", "*.cpp", "", "单文件 glob(文件名即配)"},
    };
}

// ---- 文件面数账(与旧 bench 同一套跳过规则) ---------------------------------

std::size_t CountFiles(const fs::path& root) {
    std::size_t n = 0;
    std::error_code ec;
    static const std::vector<std::string> skip = {".git", "build", "node_modules", ".evidence"};
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        std::error_code eec;
        if (it->is_directory(eec)) {
            const std::string name = PathToUtf8(it->path().filename());
            if (std::find(skip.begin(), skip.end(), name) != skip.end()) {
                it.disable_recursion_pending();
            }
        } else if (it->is_regular_file(eec)) {
            ++n;
        }
        it.increment(ec);
    }
    return n;
}

// ---- 两列执行器 ------------------------------------------------------------

struct RunOutcome {
    double wall_ms = 0.0;
    bool is_error = false;
    std::size_t output_lines = 0;
    bool truncated = false;
    ProcSample rg;               // 采样账
    std::uint64_t job_peak_bytes = 0;  // Windows direct 列的 Job Object 精确账
    std::uint64_t job_cpu_ms = 0;
};

// direct rg 列:与后端同一条 argv(纯函数直调),宿主只数字节不起解析——
// 量出来的差值即"包装开销"(起进程策略之外的解析/墙/投影都在 tool 列)。
RunOutcome RunDirectRg(const fs::path& rg_exe, const BenchQuery& q, const fs::path& target,
                       ProcessMonitor& monitor) {
    SearchRequest request;
    request.mode = q.mode == "grep" ? lubancode::tools::SearchMode::Grep
                                    : lubancode::tools::SearchMode::Glob;
    request.pattern = q.pattern;
    request.glob = q.glob;
    request.root = target;
    request.root_is_single_file = fs::is_regular_file(target);
    const lubancode::tools::SearchPolicy policy =
        lubancode::tools::BuildSearchPolicy(request);
    const lubancode::tools::RipgrepInvocation inv =
        request.mode == lubancode::tools::SearchMode::Grep
            ? BuildGrepArgv(request, policy, rg_exe)
            : BuildGlobArgv(request, policy, rg_exe);

    std::uint64_t stdout_bytes = 0;
    RunOutcome out;
    ChildProcess process;
    monitor.StartWindow();
    const auto t0 = Clock::now();
    const SpawnResult spawn = process.Start(
        PathToUtf8(rg_exe), inv.args, /*env=*/{},
        [&stdout_bytes](std::string_view chunk) {
            stdout_bytes += chunk.size();
            return true;  // 直读全量,不设墙——direct rg 跑到自然完成为准
        },
        [](std::string_view) {}, inv.cwd_utf8);
    if (!spawn.success) {
        monitor.StopWindow();
        out.is_error = true;
        out.wall_ms = ElapsedMs(t0, Clock::now());
        return out;
    }
    // 等到真退出:WaitForExit(片) 返回 false 只是"这片还没退",继续等;
    // 天花板 5 分钟防驱动自己吊死(rg 扫大树撑死秒级)。
    const auto hard_cap = t0 + std::chrono::minutes(5);
    while (!process.WaitForExit(50, nullptr)) {
        if (Clock::now() > hard_cap) {
            std::cerr << "[direct] rg 5 分钟没退,强收\n";
            break;
        }
    }
    process.Shutdown(0);
    const auto t1 = Clock::now();
    out.rg = monitor.StopWindow();
    out.wall_ms = ElapsedMs(t0, t1);
    out.output_lines = static_cast<std::size_t>(stdout_bytes);  // 直读列记字节
    const auto usage = process.ResourceUsageSnapshot();
    out.job_peak_bytes = usage.peak_memory_bytes;
    out.job_cpu_ms = usage.cpu_100ns / 10000;
    return out;
}

// SearchTool+rg 列:生产装配(exe 注入,余全真)。
RunOutcome RunTool(std::shared_ptr<SearchTool>& tool, const BenchQuery& q,
                   const fs::path& target, ProcessMonitor& monitor) {
    nlohmann::json input;
    input["mode"] = q.mode;
    input["pattern"] = q.pattern;
    input["path"] = PathToUtf8(target);
    if (!q.glob.empty()) input["glob"] = q.glob;

    RunOutcome out;
    monitor.StartWindow();
    const auto t0 = Clock::now();
    const Tool::Result result = tool->execute(input);
    const auto t1 = Clock::now();
    out.rg = monitor.StopWindow();
    out.wall_ms = ElapsedMs(t0, t1);
    out.is_error = result.is_error;
    out.output_lines = static_cast<std::size_t>(
        std::count(result.content.begin(), result.content.end(), '\n'));
    out.truncated = result.content.find("\xe6\x88\xaa\xe6\x96\xad") != std::string::npos;
    return out;
}

nlohmann::json AggregateRuns(const std::vector<RunOutcome>& runs) {
    std::vector<double> walls;
    walls.reserve(runs.size());
    for (const RunOutcome& r : runs) walls.push_back(r.wall_ms);
    std::vector<double> warm(walls.begin() + (walls.size() > 1 ? 1 : 0), walls.end());
    nlohmann::json rec;
    rec["wall_ms_samples"] = walls;
    rec["wall_ms_first_round"] = walls.front();
    rec["wall_ms_p50"] = Percentile(walls, 50);
    rec["wall_ms_p95"] = Percentile(walls, 95);
    rec["wall_ms_min"] = *std::min_element(walls.begin(), walls.end());
    rec["wall_ms_max"] = *std::max_element(walls.begin(), walls.end());
    rec["wall_ms_p50_excluding_first_round"] = warm.empty() ? 0.0 : Percentile(warm, 50);
    std::uint64_t peak = 0, cpu = 0;
    for (const RunOutcome& r : runs) {
        peak = std::max<std::uint64_t>(peak, r.rg.peak_ws);
        cpu = std::max<std::uint64_t>(cpu, r.rg.cpu_ms);
    }
    rec["rg_peak_ws_bytes_max"] = peak;
    rec["rg_cpu_ms_max"] = cpu;
    std::uint64_t job_peak = 0, job_cpu = 0;
    for (const RunOutcome& r : runs) {
        job_peak = std::max(job_peak, r.job_peak_bytes);
        job_cpu = std::max(job_cpu, r.job_cpu_ms);
    }
    if (job_peak > 0 || job_cpu > 0) {
        rec["job_peak_memory_bytes_max"] = job_peak;
        rec["job_cpu_ms_max"] = job_cpu;
    }
    rec["last_is_error"] = runs.back().is_error;
    rec["last_truncated"] = runs.back().truncated;
    rec["last_output_metric"] = runs.back().output_lines;
    return rec;
}

// ---- bench 子命令 ------------------------------------------------------------

struct TierSpec {
    std::string name;
    fs::path dir;
    bool single_file = false;
    std::vector<BenchQuery> queries;
};

int RunBench(const fs::path& rg_exe, const fs::path& out_path, int argc, char** argv) {
    std::map<std::string, std::string> opts;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        const auto eq = a.find('=');
        if (a.rfind("--", 0) == 0) {
            if (eq != std::string::npos) {
                opts[a.substr(0, eq)] = a.substr(eq + 1);
            } else if (i + 1 < argc) {
                opts[a] = argv[++i];
            }
        }
    }
    const int rounds = opts.count("--rounds") ? std::max(1, std::atoi(opts["--rounds"].c_str())) : 7;

    std::vector<TierSpec> tiers;
    if (opts.count("--small-dir")) {
        tiers.push_back({"small_dir", opts["--small-dir"], false, SmallDirQueries()});
    }
    if (opts.count("--small-file")) {
        tiers.push_back({"small_file", opts["--small-file"], true, SmallFileQueries()});
    }
    if (opts.count("--medium")) {
        tiers.push_back({"medium", opts["--medium"], false, OldBenchAlignedQueries()});
    }
    if (opts.count("--medium-p01")) {
        tiers.push_back({"medium_p01_snapshot", opts["--medium-p01"], false, OldBenchAlignedQueries()});
    }
    if (opts.count("--large")) {
        tiers.push_back({"large", opts["--large"], false, LargeCorpusQueries()});
    }
    if (tiers.empty()) {
        std::cerr << "bench 至少要给一档语料(--small-dir/--small-file/--medium/--medium-p01/--large)\n";
        return 2;
    }

    const std::string rg_name = rg_exe.filename().string();
    // 自家名下还有 rg = 上次跑挂了没收尸,如实拦;外来户(别的会话的 rg)
    // 只记账——采样与残留检测都按"父进程 == 本驱动"归属,不会串。
    const std::size_t foreign_rg = ProcessMonitor::CountForeignRg(rg_name);
    if (const std::size_t ours = ProcessMonitor::CountOurRg(rg_name); ours != 0) {
        std::cerr << "本驱动名下已有 " << ours << " 只 rg 残留(上次跑没收尸),先清场\n";
        return 2;
    }

    // 版本留档(与 doctor 同款探针,顺带校门)。
    const auto version = lubancode::tools::RunRipgrepSmoke(rg_exe);
    if (version.status != lubancode::tools::RipgrepSmokeStatus::Ready) {
        std::cerr << "rg smoke 不过: " << version.message << "\n";
        return 2;
    }

    ProcessMonitor monitor(rg_name);
    auto runner = std::make_shared<BundledRipgrepRunner>(rg_exe);
    auto tool = std::make_shared<SearchTool>(runner);

    nlohmann::json out;
    out["generated_utc"] = "see_file_mtime";
    out["platform"] =
#ifdef _WIN32
        "windows-x64"
#else
        "linux-x64"
#endif
        ;
    out["cpu_count"] = std::thread::hardware_concurrency();
    out["build_type"] =
#ifdef NDEBUG
        "release"
#else
        "debug"
#endif
        ;
    out["rounds_per_query"] = rounds;
    out["rg_version"] = version.found_version;
    out["rg_exe"] = PathToUtf8(rg_exe);
    out["foreign_rg_at_start"] = foreign_rg;
    out["tiers"] = nlohmann::json::array();

    int sanity_failures = 0;
    for (const TierSpec& tier : tiers) {
        nlohmann::json tier_rec;
        tier_rec["name"] = tier.name;
        tier_rec["dir"] = PathToUtf8(tier.dir);
        tier_rec["single_file"] = tier.single_file;
        tier_rec["file_count_scanned"] = tier.single_file ? 1 : CountFiles(tier.dir);
        tier_rec["columns"] = nlohmann::json::array();
        std::cout << "== 档位 " << tier.name << " ("
                  << tier_rec["file_count_scanned"].get<std::size_t>() << " 文件) ==\n";

        for (const char* column : {"direct_rg", "tool"}) {
            nlohmann::json col_rec;
            col_rec["column"] = column;
            col_rec["queries"] = nlohmann::json::array();
            for (const BenchQuery& q : tier.queries) {
                std::vector<RunOutcome> runs;
                runs.reserve(rounds);
                for (int i = 0; i < rounds; ++i) {
                    runs.push_back(std::string(column) == "direct_rg"
                                       ? RunDirectRg(rg_exe, q, tier.dir, monitor)
                                       : RunTool(tool, q, tier.dir, monitor));
                }
                nlohmann::json rec = AggregateRuns(runs);
                rec["id"] = q.id;
                rec["mode"] = q.mode;
                rec["pattern"] = q.pattern;
                rec["glob"] = q.glob;
                rec["note"] = q.note;
                col_rec["queries"].push_back(rec);
                std::cout << "  " << column << " " << q.id
                          << ": p50=" << rec["wall_ms_p50"].get<double>()
                          << "ms p95=" << rec["wall_ms_p95"].get<double>()
                          << "ms rg_peak=" << rec["rg_peak_ws_bytes_max"].get<std::uint64_t>()
                          << "B err=" << rec["last_is_error"].get<bool>() << "\n";
                // 语料/探针对不对得上,当场报,不带着假数往下跑。
                if (column == std::string("tool")) {
                    if (q.id == "no_match" &&
                        (rec["last_is_error"].get<bool>() ||
                         rec["last_output_metric"].get<std::size_t>() != 0)) {
                        std::cerr << "  [sanity] no_match 却有输出/报错——语料或针不对\n";
                        ++sanity_failures;
                    }
                    if ((q.id == "literal_common_word" || q.id == "glob_enum_cpp" ||
                         q.id == "glob_enum_txt") &&
                        !rec["last_is_error"].get<bool>() &&
                        rec["last_output_metric"].get<std::size_t>() == 0) {
                        std::cerr << "  [sanity] " << q.id << " 零输出——语料里没这根针?\n";
                        ++sanity_failures;
                    }
                }
            }
            tier_rec["columns"].push_back(col_rec);
        }
        out["tiers"].push_back(tier_rec);
    }

    const HostSnapshot host = ProcessMonitor::Host();
    out["host_after_bench"] = {
        {"peak_ws_bytes", host.peak_ws},
        {"handles", host.handles},
        {"threads", host.threads},
    };

    fs::create_directories(out_path.parent_path());
    std::ofstream f(out_path, std::ios::binary);
    f << out.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
    f.close();
    std::cout << "bench 写入: " << PathToUtf8(out_path) << "\n";

    if (opts.count("--csv")) {
        const fs::path csv_path(opts["--csv"]);
        fs::create_directories(csv_path.parent_path());
        std::ofstream c(csv_path, std::ios::binary);
        c << "tier,file_count,query_id,mode,pattern,column,rounds,wall_ms_first,"
             "wall_ms_p50,warm_p50,wall_ms_p95,wall_ms_min,wall_ms_max,last_is_error,"
             "last_truncated,last_output_metric,rg_peak_ws_bytes_max,rg_cpu_ms_max,"
             "job_peak_memory_bytes_max,job_cpu_ms_max\n";
        for (const auto& tier : out["tiers"]) {
            for (const auto& col : tier["columns"]) {
                for (const auto& q : col["queries"]) {
                    c << tier["name"].get<std::string>() << ","
                      << tier["file_count_scanned"].get<std::size_t>() << ","
                      << q["id"].get<std::string>() << "," << q["mode"].get<std::string>()
                      << ",\"" << q["pattern"].get<std::string>() << "\","
                      << col["column"].get<std::string>() << ","
                      << q["wall_ms_samples"].size() << ","
                      << q["wall_ms_first_round"].get<double>() << ","
                      << q["wall_ms_p50"].get<double>() << ","
                      << q["wall_ms_p50_excluding_first_round"].get<double>() << ","
                      << q["wall_ms_p95"].get<double>() << ","
                      << q["wall_ms_min"].get<double>() << ","
                      << q["wall_ms_max"].get<double>() << ","
                      << q["last_is_error"].get<bool>() << ","
                      << q["last_truncated"].get<bool>() << ","
                      << q["last_output_metric"].get<std::size_t>() << ","
                      << q["rg_peak_ws_bytes_max"].get<std::uint64_t>() << ","
                      << q["rg_cpu_ms_max"].get<std::uint64_t>();
                    if (q.contains("job_peak_memory_bytes_max")) {
                        c << "," << q["job_peak_memory_bytes_max"].get<std::uint64_t>()
                          << "," << q["job_cpu_ms_max"].get<std::uint64_t>();
                    } else {
                        c << ",";
                    }
                    c << "\n";
                }
            }
        }
        std::cout << "csv 写入: " << PathToUtf8(csv_path) << "\n";
    }
    return sanity_failures == 0 ? 0 : 3;
}

// ---- stress 子命令 ------------------------------------------------------------

int RunStress(const fs::path& rg_exe, const fs::path& out_path, int argc, char** argv) {
    std::map<std::string, std::string> opts;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        const auto eq = a.find('=');
        if (a.rfind("--", 0) == 0) {
            if (eq != std::string::npos) opts[a.substr(0, eq)] = a.substr(eq + 1);
            else if (i + 1 < argc) opts[a] = argv[++i];
        }
    }
    if (!opts.count("--corpus")) {
        std::cerr << "stress 需要 --corpus(短搜索语料目录)\n";
        return 2;
    }
    const int rounds = opts.count("--rounds") ? std::atoi(opts["--rounds"].c_str()) : 1000;
    const int large_rounds =
        opts.count("--large-rounds") ? std::atoi(opts["--large-rounds"].c_str()) : 100;
    const long long cap_ms =
        opts.count("--round-cap-ms") ? std::atoll(opts["--round-cap-ms"].c_str()) : 30000;
    const long long large_cap_ms =
        opts.count("--large-round-cap-ms") ? std::atoll(opts["--large-round-cap-ms"].c_str())
                                           : 180000;

    const std::string rg_name = rg_exe.filename().string();
    if (const std::size_t ours = ProcessMonitor::CountOurRg(rg_name); ours != 0) {
        std::cerr << "本驱动名下已有 " << ours << " 只 rg 残留(上次跑没收尸),先清场\n";
        return 2;
    }
    const std::size_t foreign_rg = ProcessMonitor::CountForeignRg(rg_name);
    const auto smoke = lubancode::tools::RunRipgrepSmoke(rg_exe);
    if (smoke.status != lubancode::tools::RipgrepSmokeStatus::Ready) {
        std::cerr << "rg smoke 不过: " << smoke.message << "\n";
        return 2;
    }

    ProcessMonitor monitor(rg_name);
    auto tool = std::make_shared<SearchTool>(std::make_shared<BundledRipgrepRunner>(rg_exe));

    nlohmann::json out;
    out["rg_version"] = smoke.found_version;
    out["round_cap_ms"] = cap_ms;
    out["foreign_rg_at_start"] = foreign_rg;
    out["phases"] = nlohmann::json::array();
    int failures = 0;

    const HostSnapshot before = ProcessMonitor::Host();

    const std::vector<BenchQuery> short_queries = SmallDirQueries();
    {
        const fs::path corpus(opts["--corpus"]);
        std::size_t errors = 0, residue_checks = 0, residue_bad = 0;
        std::vector<double> walls;
        walls.reserve(rounds);
        const auto t0 = Clock::now();
        for (int i = 0; i < rounds; ++i) {
            const BenchQuery& q = short_queries[i % short_queries.size()];
            RunOutcome r = RunTool(tool, q, corpus, monitor);
            walls.push_back(r.wall_ms);
            if (r.wall_ms > static_cast<double>(cap_ms)) {
                std::cerr << "[挂死?] 第 " << i << " 轮 " << q.id << " 耗时 "
                          << r.wall_ms << "ms 超帽 " << cap_ms << "ms\n";
                ++failures;
                break;
            }
            if (r.is_error) {
                ++errors;
                std::cerr << "[报错] 第 " << i << " 轮 " << q.id << "\n";
                if (errors > 10) { std::cerr << "报错过多,停\n"; ++failures; break; }
            }
            if ((i + 1) % 100 == 0) {
                const std::size_t residue = ProcessMonitor::CountOurRg(rg_name);
                ++residue_checks;
                if (residue != 0) {
                    std::cerr << "[孤儿] 第 " << i + 1 << " 轮后进程表残留 rg x" << residue << "\n";
                    ++residue_bad;
                    ++failures;
                }
                const HostSnapshot mid = ProcessMonitor::Host();
                std::cout << "  短搜 " << (i + 1) << "/" << rounds
                          << " handles=" << mid.handles << " threads=" << mid.threads << "\n";
            }
        }
        const double total_s = ElapsedMs(t0, Clock::now()) / 1000.0;
        nlohmann::json phase;
        phase["name"] = "short_x" + std::to_string(rounds);
        phase["corpus"] = PathToUtf8(corpus);
        phase["rounds_done"] = walls.size();
        phase["wall_s_total"] = total_s;
        if (!walls.empty()) {
            phase["wall_ms_p50"] = Percentile(walls, 50);
            phase["wall_ms_p95"] = Percentile(walls, 95);
            phase["wall_ms_max"] = *std::max_element(walls.begin(), walls.end());
        }
        phase["is_error_rounds"] = errors;
        phase["residue_checks"] = residue_checks;
        phase["residue_bad_checks"] = residue_bad;
        out["phases"].push_back(phase);
        std::cout << "短搜 " << walls.size() << " 轮完,共 " << total_s << "s,"
                  << " p50=" << phase.value("wall_ms_p50", 0.0) << "ms\n";
    }

    if (opts.count("--large") && large_rounds > 0) {
        const fs::path large(opts["--large"]);
        const std::vector<BenchQuery> qs = LargeCorpusQueries();
        std::vector<double> walls;
        std::size_t errors = 0, residue_bad = 0, truncations = 0;
        const auto t0 = Clock::now();
        for (int i = 0; i < large_rounds; ++i) {
            const BenchQuery& q = qs[i % qs.size()];
            RunOutcome r = RunTool(tool, q, large, monitor);
            walls.push_back(r.wall_ms);
            if (r.truncated) ++truncations;
            if (r.wall_ms > static_cast<double>(large_cap_ms)) {
                std::cerr << "[挂死?] 大树第 " << i << " 轮 " << q.id << " 耗时 "
                          << r.wall_ms << "ms\n";
                ++failures;
                break;
            }
            if (r.is_error) {
                ++errors;
                std::cerr << "[报错] 大树第 " << i << " 轮 " << q.id << "\n";
                if (errors > 5) { ++failures; break; }
            }
            if ((i + 1) % 25 == 0) {
                if (ProcessMonitor::CountOurRg(rg_name) != 0) {
                    std::cerr << "[孤儿] 大树第 " << i + 1 << " 轮后残留 rg\n";
                    ++residue_bad;
                    ++failures;
                }
                std::cout << "  大树 " << (i + 1) << "/" << large_rounds << "\n";
            }
        }
        const double total_s = ElapsedMs(t0, Clock::now()) / 1000.0;
        nlohmann::json phase;
        phase["name"] = "large_tree_x" + std::to_string(large_rounds);
        phase["corpus"] = PathToUtf8(large);
        phase["rounds_done"] = walls.size();
        phase["wall_s_total"] = total_s;
        if (!walls.empty()) {
            phase["wall_ms_p50"] = Percentile(walls, 50);
            phase["wall_ms_p95"] = Percentile(walls, 95);
            phase["wall_ms_max"] = *std::max_element(walls.begin(), walls.end());
        }
        phase["truncated_rounds"] = truncations;
        phase["is_error_rounds"] = errors;
        phase["residue_bad_checks"] = residue_bad;
        out["phases"].push_back(phase);
        std::cout << "大树 " << walls.size() << " 轮完,共 " << total_s << "s,"
                  << " 截断轮 " << truncations << "\n";
    }

    const HostSnapshot after = ProcessMonitor::Host();
    const std::size_t residue_final = ProcessMonitor::CountOurRg(rg_name);
    out["host_before"] = {{"handles", before.handles}, {"threads", before.threads}};
    out["host_after"] = {{"handles", after.handles}, {"threads", after.threads},
                         {"peak_ws_bytes", after.peak_ws}};
    out["handle_delta"] = after.handles > before.handles
                              ? after.handles - before.handles : 0;
    out["thread_delta"] = after.threads > before.threads ? after.threads - before.threads : 0;
    out["rg_residue_final"] = residue_final;

    // 判门:不崩(无报错轮)、不挂(无超帽轮)、句柄/线程不单调膨胀、无孤儿。
    if (out["handle_delta"].get<std::uint64_t>() > 50) {
        std::cerr << "[泄漏嫌疑] 句柄涨了 " << out["handle_delta"] << "\n";
        ++failures;
    }
    if (out["thread_delta"].get<std::uint64_t>() > 2) {
        std::cerr << "[泄漏嫌疑] 线程涨了 " << out["thread_delta"] << "\n";
        ++failures;
    }
    if (residue_final != 0) {
        std::cerr << "[孤儿] 收尾进程表还有 rg x" << residue_final << "\n";
        ++failures;
    }

    fs::create_directories(out_path.parent_path());
    std::ofstream f(out_path, std::ios::binary);
    f << out.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
    std::cout << "stress 写入: " << PathToUtf8(out_path)
              << (failures == 0 ? "  [过门]" : "  [未过门]") << "\n";
    return failures == 0 ? 0 : 1;
}

// ---- cancel 子命令 ------------------------------------------------------------

int RunCancel(const fs::path& rg_exe, const fs::path& out_path, int argc, char** argv) {
    std::map<std::string, std::string> opts;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        const auto eq = a.find('=');
        if (a.rfind("--", 0) == 0) {
            if (eq != std::string::npos) opts[a.substr(0, eq)] = a.substr(eq + 1);
            else if (i + 1 < argc) opts[a] = argv[++i];
        }
    }
    if (!opts.count("--corpus")) {
        std::cerr << "cancel 需要 --corpus(要足够大,保证按停时搜索还在跑)\n";
        return 2;
    }
    const int rounds = opts.count("--rounds") ? std::atoi(opts["--rounds"].c_str()) : 10;
    const int delay_ms = opts.count("--delay-ms") ? std::atoi(opts["--delay-ms"].c_str()) : 200;

    const std::string rg_name = rg_exe.filename().string();
    if (const std::size_t ours = ProcessMonitor::CountOurRg(rg_name); ours != 0) {
        std::cerr << "本驱动名下已有 " << ours << " 只 rg 残留(上次跑没收尸),先清场\n";
        return 2;
    }
    const auto smoke = lubancode::tools::RunRipgrepSmoke(rg_exe);
    if (smoke.status != lubancode::tools::RipgrepSmokeStatus::Ready) {
        std::cerr << "rg smoke 不过: " << smoke.message << "\n";
        return 2;
    }

    ProcessMonitor monitor(rg_name);
    auto tool = std::make_shared<SearchTool>(std::make_shared<BundledRipgrepRunner>(rg_exe));
    const fs::path corpus(opts["--corpus"]);

    nlohmann::json out;
    out["rg_version"] = smoke.found_version;
    out["delay_ms"] = delay_ms;
    out["rounds"] = rounds;
    out["midflight"] = nlohmann::json::array();
    int failures = 0;

    nlohmann::json input;
    input["mode"] = "grep";
    input["pattern"] = "definitely_absent_zzz_token_12345";
    input["path"] = PathToUtf8(corpus);

    // 起跑前取消:不起进程,量纯收口开销。
    {
        std::atomic<bool> cancel{true};
        lubancode::tools::ToolExecutionContext ctx;
        ctx.cancel = &cancel;
        const auto t0 = Clock::now();
        const Tool::Result r = tool->execute(input, ctx);
        const double ms = ElapsedMs(t0, Clock::now());
        const bool ok = r.is_error && r.content.find("search_cancelled") != std::string::npos;
        out["precancelled"] = {{"wall_ms", ms}, {"ok", ok}};
        if (!ok) ++failures;
        std::cout << "起跑前取消: " << ms << "ms ok=" << ok << "\n";
    }

    // 不取消对照:全时长。
    {
        const auto t0 = Clock::now();
        const Tool::Result r = tool->execute(input);
        const double ms = ElapsedMs(t0, Clock::now());
        out["no_cancel_full_ms"] = ms;
        std::cout << "不取消对照全时长: " << ms << "ms\n";
    }

    std::vector<double> return_lat, clean_lat;
    for (int i = 0; i < rounds; ++i) {
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
        std::string content;
        bool is_error = false;
        std::thread worker([&] {
            lubancode::tools::ToolExecutionContext ctx;
            ctx.cancel = &cancel;
            const Tool::Result r = tool->execute(input, ctx);
            content = r.content;
            is_error = r.is_error;
            done.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        const auto t_cancel = Clock::now();
        cancel.store(true, std::memory_order_relaxed);
        // 等返回:execute 回来才算"取消响应"。
        while (!done.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto t_return = Clock::now();
        // 等进程表清干净:按停到无残留。
        auto t_clean = t_return;
        while (ProcessMonitor::CountOurRg(rg_name) != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            t_clean = Clock::now();
            if (ElapsedMs(t_cancel, t_clean) > 10000.0) break;
        }
        worker.join();
        const double ret_ms = ElapsedMs(t_cancel, t_return);
        const double cln_ms = ElapsedMs(t_cancel, t_clean);
        return_lat.push_back(ret_ms);
        clean_lat.push_back(cln_ms);
        const bool cancelled_ok =
            is_error && content.find("search_cancelled") != std::string::npos;
        const bool residue = ProcessMonitor::CountOurRg(rg_name) != 0;
        out["midflight"].push_back({{"round", i},
                                    {"cancel_to_return_ms", ret_ms},
                                    {"cancel_to_clean_table_ms", cln_ms},
                                    {"cancelled_ok", cancelled_ok},
                                    {"residue_after", residue}});
        if (!cancelled_ok) {
            std::cerr << "[取消未达] 第 " << i << " 轮终态不是 search_cancelled\n";
            ++failures;
        }
        if (residue) {
            std::cerr << "[残留] 第 " << i << " 轮收尾进程表还有 rg\n";
            ++failures;
        }
        std::cout << "按停第 " << i << " 轮: 返回 " << ret_ms << "ms / 表净 "
                  << cln_ms << "ms\n";
    }
    out["cancel_to_return_ms_p50"] = Percentile(return_lat, 50);
    out["cancel_to_return_ms_p95"] = Percentile(return_lat, 95);
    out["cancel_to_return_ms_max"] = *std::max_element(return_lat.begin(), return_lat.end());
    out["cancel_to_clean_ms_p50"] = Percentile(clean_lat, 50);
    out["cancel_to_clean_ms_p95"] = Percentile(clean_lat, 95);
    out["cancel_to_clean_ms_max"] = *std::max_element(clean_lat.begin(), clean_lat.end());

    fs::create_directories(out_path.parent_path());
    std::ofstream f(out_path, std::ios::binary);
    f << out.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
    std::cout << "cancel 写入: " << PathToUtf8(out_path)
              << (failures == 0 ? "  [过门]" : "  [未过门]") << "\n";
    return failures == 0 ? 0 : 1;
}

// ---- report 子命令 ------------------------------------------------------------

const double kGateRatio = 0.20;   // SearchTool+rg 不得比 direct rg 多 20% wall
const double kGateAbsMs = 20.0;   // ……或 20ms,取较宽者

int RunReport(const fs::path& out_path, const std::vector<std::string>& args) {
    std::vector<std::string> files;
    std::string csv;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--csv" && i + 1 < args.size()) csv = args[++i];
        else files.push_back(a);
    }
    if (files.size() < 2) {
        std::cerr << "report 需要 <旧内核bench.json> <新raw.json...>\n";
        return 2;
    }

    std::ifstream old_file(files[0], std::ios::binary);
    if (!old_file.is_open()) {
        std::cerr << "旧 bench 打不开: " << files[0] << "\n";
        return 2;
    }
    const nlohmann::json old_bench = nlohmann::json::parse(old_file);

    nlohmann::json out;
    out["old_bench_file"] = files[0];
    out["old_bench_target_dir"] = old_bench.value("target_dir", "");
    out["old_bench_file_count"] = old_bench.value("file_count_scanned", 0);
    out["runs"] = nlohmann::json::array();

    // 旧内核账按 query id 建(只对 medium_p01_snapshot 档——同一份语料快照)。
    std::map<std::string, const nlohmann::json*> old_by_id;
    for (const auto& q : old_bench["queries"]) {
        old_by_id[q["id"].get<std::string>()] = &q;
    }

    for (std::size_t fi = 1; fi < files.size(); ++fi) {
        std::ifstream in(files[fi], std::ios::binary);
        if (!in.is_open()) {
            std::cerr << "raw 打不开: " << files[fi] << "\n";
            return 2;
        }
        const nlohmann::json raw = nlohmann::json::parse(in);
        nlohmann::json run;
        run["file"] = files[fi];
        run["platform"] = raw.value("platform", "");
        run["rg_version"] = raw.value("rg_version", "");
        run["tiers"] = nlohmann::json::array();

        for (const auto& tier : raw["tiers"]) {
            nlohmann::json tier_rec;
            tier_rec["name"] = tier["name"];
            tier_rec["file_count"] = tier["file_count_scanned"];
            tier_rec["queries"] = nlohmann::json::array();

            // 同档内取两列。
            const nlohmann::json* direct = nullptr;
            const nlohmann::json* tool = nullptr;
            for (const auto& col : tier["columns"]) {
                if (col["column"] == "direct_rg") direct = &col;
                if (col["column"] == "tool") tool = &col;
            }
            if (direct == nullptr || tool == nullptr) continue;
            const std::map<std::string, const nlohmann::json*> direct_by_id = [&] {
                std::map<std::string, const nlohmann::json*> m;
                for (const auto& q : (*direct)["queries"]) m[q["id"]] = &q;
                return m;
            }();
            const std::map<std::string, const nlohmann::json*> tool_by_id = [&] {
                std::map<std::string, const nlohmann::json*> m;
                for (const auto& q : (*tool)["queries"]) m[q["id"]] = &q;
                return m;
            }();

            for (const auto& [id, tq] : tool_by_id) {
                nlohmann::json rec;
                rec["id"] = id;
                rec["tool_p50"] = (*tq)["wall_ms_p50"];
                rec["tool_p95"] = (*tq)["wall_ms_p95"];
                rec["truncated"] = (*tq)["last_truncated"];
                const auto dit = direct_by_id.find(id);
                if (dit != direct_by_id.end()) {
                    const double d_p50 = (*dit->second)["wall_ms_p50"].get<double>();
                    const double t_p50 = (*tq)["wall_ms_p50"].get<double>();
                    const double allowance = std::max(kGateRatio * d_p50, kGateAbsMs);
                    rec["direct_p50"] = d_p50;
                    rec["direct_p95"] = (*dit->second)["wall_ms_p95"];
                    rec["overhead_ms"] = t_p50 - d_p50;
                    rec["overhead_ratio"] = d_p50 > 0 ? t_p50 / d_p50 : 0.0;
                    rec["gate_allowance_ms"] = allowance;
                    rec["gate1_pass"] = (t_p50 - d_p50) <= allowance + 1e-9;
                    // 包装开销的真同工判读:工具列主动停树的档(截断),direct
                    // 跑完全程,direct 做的功更多——开销差为负不说明包装免费,
                    // 标注出来,门判照算但注明口径。
                    rec["comparison_caveat"] = (*tq)["last_truncated"].get<bool>()
                        ? "tool 100 条截断早停,direct 全程,开销差含早停红利"
                        : "两侧同跑全程,纯包装开销";
                }
                const auto oit = old_by_id.find(id);
                const bool is_p01_tier =
                    tier["name"].get<std::string>() == "medium_p01_snapshot";
                if (oit != old_by_id.end() && is_p01_tier) {
                    const double o_p50 = (*oit->second)["wall_ms_p50"].get<double>();
                    const double n_p50 = (*tq)["wall_ms_p50"].get<double>();
                    rec["old_kernel_p50"] = o_p50;
                    rec["old_kernel_p95"] = (*oit->second)["wall_ms_p95"];
                    rec["speedup_old_vs_new_p50"] = n_p50 > 0 ? o_p50 / n_p50 : 0.0;
                    rec["speedup_old_vs_new_p95"] =
                        (*tq)["wall_ms_p95"].get<double>() > 0
                            ? (*oit->second)["wall_ms_p95"].get<double>() /
                                  (*tq)["wall_ms_p95"].get<double>()
                            : 0.0;
                }
                tier_rec["queries"].push_back(rec);
            }

            // 门 2:大树高频命中第 100 条主动停,耗时不随语料线性长。
            const auto it_hi = tool_by_id.find("high_frequency");
            const auto it_no = tool_by_id.find("no_match");
            if (it_hi != tool_by_id.end()) {
                tier_rec["gate2_truncated_at_100"] =
                    (*it_hi->second)["last_truncated"].get<bool>() &&
                    (*it_hi->second)["last_output_metric"].get<std::size_t>() >= 100;
                if (it_no != tool_by_id.end()) {
                    const double hi = (*it_hi->second)["wall_ms_p50"].get<double>();
                    const double no = (*it_no->second)["wall_ms_p50"].get<double>();
                    tier_rec["gate2_highfreq_vs_fullscan_p50_ratio"] = no > 0 ? hi / no : 0.0;
                }
            }
            run["tiers"].push_back(tier_rec);
        }
        out["runs"].push_back(run);
    }

    // 跨档线性度:high_frequency 截断耗时 vs 文件数(不随语料线性增长即过)。
    nlohmann::json linearity = nlohmann::json::array();
    for (auto& run : out["runs"]) {
        const nlohmann::json* small = nullptr;
        const nlohmann::json* large = nullptr;
        for (const auto& t : run["tiers"]) {
            const std::string n = t["name"];
            if (n == "medium" || n == "small_dir") small = &t;
            if (n == "large") large = &t;
        }
        if (small == nullptr || large == nullptr) continue;
        const double files_ratio =
            large->at("file_count").get<double>() /
            std::max<double>(1.0, small->at("file_count").get<double>());
        auto p50_of = [](const nlohmann::json& t, const char* id) -> double {
            for (const auto& q : t.at("queries")) {
                if (q["id"] == id) return q["tool_p50"].get<double>();
            }
            return -1;
        };
        const double hi_s = p50_of(*small, "high_frequency");
        const double hi_l = p50_of(*large, "high_frequency");
        if (hi_s > 0 && hi_l > 0) {
            nlohmann::json row;
            row["file_ratio_large_vs_small"] = files_ratio;
            row["highfreq_p50_ratio_large_vs_small"] = hi_l / hi_s;
            row["sublinear"] = (hi_l / hi_s) < files_ratio / 10.0;
            linearity.push_back(row);
        }
    }
    out["gate2_linearity"] = linearity;

    fs::create_directories(out_path.parent_path());
    std::ofstream f(out_path, std::ios::binary);
    f << out.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";

    // 人读表(控制台),数字全从 JSON 来。
    for (const auto& run : out["runs"]) {
        std::cout << "\n== " << run["file"].get<std::string>() << " ("
                  << run["platform"].get<std::string>() << ") ==\n";
        for (const auto& tier : run["tiers"]) {
            std::cout << "-- 档 " << tier["name"].get<std::string>() << " ("
                      << tier["file_count"].get<std::size_t>() << " 文件) --\n";
            std::cout << "id                        tool_p50   direct_p50   old_p50   "
                         "overhead_ms  gate1  speedup(old/new)\n";
            for (const auto& q : tier["queries"]) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%-24s %9.1f %12.1f %9.1f %12.1f %6s %10.1fx",
                              q["id"].get<std::string>().c_str(),
                              q["tool_p50"].get<double>(),
                              q.value("direct_p50", -1.0),
                              q.value("old_kernel_p50", -1.0),
                              q.value("overhead_ms", -999.0),
                              q.value("gate1_pass", false) ? "PASS" : "FAIL",
                              q.value("speedup_old_vs_new_p50", -1.0));
                std::cout << buf << "\n";
            }
        }
    }
    for (const auto& row : linearity) {
        std::cout << "门2线性度: 文件比 " << row["file_ratio_large_vs_small"]
                  << "x,高频截断耗时比 " << row["highfreq_p50_ratio_large_vs_small"]
                  << "x, 亚线性=" << row["sublinear"].get<bool>() << "\n";
    }

    if (!csv.empty()) {
        const fs::path csv_path(csv);
        fs::create_directories(csv_path.parent_path());
        std::ofstream c(csv_path, std::ios::binary);
        // 数字列可能有缺(无 direct 对手/无旧内核对账),缺了写空格串。
        const auto cell = [](const nlohmann::json& q, const char* key) -> std::string {
            return q.contains(key) ? q[key].dump() : std::string();
        };
        c << "platform,tier,file_count,query_id,tool_p50_ms,tool_p95_ms,direct_p50_ms,"
             "direct_p95_ms,overhead_ms,overhead_ratio,gate_allowance_ms,gate1_pass,"
             "old_kernel_p50_ms,old_kernel_p95_ms,speedup_p50,speedup_p95,truncated,"
             "comparison_caveat\n";
        for (const auto& run : out["runs"]) {
            for (const auto& tier : run["tiers"]) {
                for (const auto& q : tier["queries"]) {
                    c << run["platform"].get<std::string>() << ","
                      << tier["name"].get<std::string>() << ","
                      << tier["file_count"].get<std::size_t>() << ","
                      << q["id"].get<std::string>() << ","
                      << q["tool_p50"].get<double>() << ","
                      << q["tool_p95"].get<double>() << ","
                      << cell(q, "direct_p50") << ","
                      << cell(q, "direct_p95") << ","
                      << cell(q, "overhead_ms") << ","
                      << cell(q, "overhead_ratio") << ","
                      << cell(q, "gate_allowance_ms") << ","
                      << (q.value("gate1_pass", false) ? 1 : 0) << ","
                      << cell(q, "old_kernel_p50") << ","
                      << cell(q, "old_kernel_p95") << ","
                      << cell(q, "speedup_old_vs_new_p50") << ","
                      << cell(q, "speedup_old_vs_new_p95") << ","
                      << (q["truncated"].get<bool>() ? 1 : 0) << ",\""
                      << q.value("comparison_caveat", "") << "\"\n";
                }
            }
        }
        std::cout << "csv 写入: " << PathToUtf8(csv_path) << "\n";
    }
    std::cout << "报告写入: " << PathToUtf8(out_path) << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "用法:\n"
                  << "  search_perf_driver bench  <rg路径> <输出JSON> [--small-dir D] "
                     "[--small-file F] [--medium D] [--medium-p01 D] [--large D] "
                     "[--rounds N] [--csv PATH]\n"
                  << "  search_perf_driver stress <rg路径> <输出JSON> --corpus D "
                     "[--rounds 1000] [--large D] [--large-rounds 100] [--round-cap-ms N]\n"
                  << "  search_perf_driver cancel <rg路径> <输出JSON> --corpus D "
                     "[--delay-ms 200] [--rounds 10]\n"
                  << "  search_perf_driver report <旧内核bench.json> <新raw.json...> "
                     "--out <报告JSON> [--csv PATH]\n";
        return 2;
    }
    const std::string sub = argv[1];
    try {
        if (sub == "bench") {
            if (argc < 4) return 2;
            return RunBench(Utf8ToPath(argv[2]), Utf8ToPath(argv[3]), argc - 4, argv + 4);
        }
        if (sub == "stress") {
            if (argc < 4) return 2;
            return RunStress(Utf8ToPath(argv[2]), Utf8ToPath(argv[3]), argc - 4, argv + 4);
        }
        if (sub == "cancel") {
            if (argc < 4) return 2;
            return RunCancel(Utf8ToPath(argv[2]), Utf8ToPath(argv[3]), argc - 4, argv + 4);
        }
        if (sub == "report") {
            // report 的 <旧bench> <raw...> --out <json> [--csv p]
            std::string out_path;
            std::vector<std::string> rest;
            for (int i = 2; i < argc; ++i) {
                const std::string a = argv[i];
                if (a == "--out" && i + 1 < argc) out_path = argv[++i];
                else rest.push_back(a);
            }
            if (out_path.empty()) {
                std::cerr << "report 需要 --out <报告JSON>\n";
                return 2;
            }
            return RunReport(Utf8ToPath(out_path), rest);
        }
    } catch (const std::exception& e) {
        std::cerr << "异常: " << e.what() << "\n";
        return 4;
    }
    std::cerr << "未知子命令: " << sub << "\n";
    return 2;
}
