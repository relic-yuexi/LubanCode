#include "tools/background_tasks.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace lubancode::tools {

namespace {

// 探活轮询间隔。200ms 对"人感知不到延迟 + 不烧 CPU"两头都够用——后台命令
// 通常是秒级以上的长跑任务,差这一拍没人察觉。
constexpr int kPollIntervalMs = 200;

// ReadOutput 一次读多少字节上限。日志文件可能被长命进程写得很大,读全文
// 不限会吃光内存;超过就只取末尾这一段(尾部正是最新输出,最该看的部分)。
constexpr std::streamoff kReadCapBytes = 64 * 1024;

// 把一条任务的状态翻成人能读的中文标签(给结果文本/通知用)。
const char* StatusLabel(BackgroundTaskStatus s) {
    switch (s) {
        case BackgroundTaskStatus::Running: return "运行中";
        case BackgroundTaskStatus::Completed: return "完成(退出码 0)";
        case BackgroundTaskStatus::Failed: return "失败(非零退出码)";
        case BackgroundTaskStatus::Stopped: return "已停止";
    }
    return "未知";
}

}  // namespace

BackgroundTaskRegistry& BackgroundTaskRegistry::Instance() {
    // magic static:线程安全初始化,第一次调用才构造,程序退出时自动析构
    // (析构里 stop_all_ 置位 + join 所有 watcher,把后台监听线程收干净)。
    static BackgroundTaskRegistry instance;
    return instance;
}

BackgroundTaskRegistry::~BackgroundTaskRegistry() {
    // 跟所有 watcher 打招呼"别再探了",然后逐个 join。join 不持 mutex_——
    // watcher 最后一次抢锁释放后看到 stop_all_ 就 return,主线程这一头干等
    // 它退出即可,不会死锁(临界区里只改了几个字段,极短)。
    stop_all_.store(true);
    for (auto& entry : entries_) {
        if (entry->watcher.joinable()) {
            entry->watcher.join();
        }
    }
}

std::string BackgroundTaskRegistry::Register(std::string command, std::string shell, unsigned long pid,
                                              std::string log_path) {
    auto entry = std::make_unique<Entry>();
    entry->info.task_id = std::to_string(next_id_.fetch_add(1));
    entry->info.command = std::move(command);
    entry->info.shell = std::move(shell);
    entry->info.pid = pid;
    entry->info.log_path = std::move(log_path);
    entry->info.status = BackgroundTaskStatus::Running;
    entry->info.start_time = std::chrono::system_clock::now();

    const std::string task_id = entry->info.task_id;

    // watcher 线程得在 entry 落进 vector 之后再起——它一启动就会拿着 task_id
    // 去表里找自己。这里先 std::move(thread) 进 entry,再把 entry push 进表,
    // 顺序很关键:thread 对象一旦构造就开始跑了,所以 entry 得先就位。
    entry->watcher = std::thread([this, task_id] { WatchThread(task_id); });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(std::move(entry));
    }
    return task_id;
}

std::vector<BackgroundTaskInfo> BackgroundTaskRegistry::List() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BackgroundTaskInfo> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        out.push_back(entry->info);
    }
    // 按 task_id 数字升序排(List 给人看,顺序稳定好认)。
    std::sort(out.begin(), out.end(),
              [](const BackgroundTaskInfo& a, const BackgroundTaskInfo& b) {
                  return std::stoull(a.task_id) < std::stoull(b.task_id);
              });
    return out;
}

std::optional<BackgroundTaskInfo> BackgroundTaskRegistry::Get(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) {
        if (entry->info.task_id == task_id) {
            return entry->info;
        }
    }
    return std::nullopt;
}

std::string BackgroundTaskRegistry::ReadOutput(const std::string& task_id, int tail_lines) {
    std::string log_path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : entries_) {
            if (entry->info.task_id == task_id) {
                log_path = entry->info.log_path;
                break;
            }
        }
    }
    if (log_path.empty()) {
        return std::string();  // task_id 不认得
    }

    // 日志文件可能正在被后台进程写,以 ate(定位到末尾)方式打开,读文件大小
    // 后决定是从头读还是只取末尾 kReadCapBytes。
    std::ifstream file(log_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return std::string();  // 文件读不了(可能刚被清理/权限问题)
    }
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return std::string();  // 空文件/还没写东西
    }

    std::string data;
    const std::streamoff read_from = (size > kReadCapBytes) ? (size - kReadCapBytes) : 0;
    file.seekg(read_from, std::ios::beg);
    data.resize(static_cast<std::size_t>(size - read_from));
    file.read(data.data(), data.size());
    if (file.gcount() <= 0) {
        return std::string();
    }
    data.resize(static_cast<std::size_t>(file.gcount()));

    if (tail_lines <= 0) {
        return data;  // 全文(已截断到末尾 64KB)
    }

    // 按行切,取末尾 tail_lines 行。
    std::vector<std::string> lines;
    {
        std::string current;
        for (char c : data) {
            if (c == '\n') {
                current.push_back('\n');
                lines.push_back(std::move(current));
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) {
            lines.push_back(std::move(current));  // 末尾不带回车的残行
        }
    }
    if (static_cast<int>(lines.size()) <= tail_lines) {
        std::ostringstream oss;
        for (const auto& l : lines) {
            oss << l;
        }
        return oss.str();
    }
    std::ostringstream oss;
    const std::size_t start = lines.size() - static_cast<std::size_t>(tail_lines);
    for (std::size_t i = start; i < lines.size(); ++i) {
        oss << lines[i];
    }
    return oss.str();
}

bool BackgroundTaskRegistry::Stop(const std::string& task_id) {
    unsigned long pid = 0;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : entries_) {
            if (entry->info.task_id == task_id) {
                found = true;
                if (entry->info.status != BackgroundTaskStatus::Running) {
                    return true;  // 已终态,不重复杀
                }
                pid = entry->info.pid;
                break;
            }
        }
    }
    if (!found || pid == 0) {
        return false;
    }

#ifdef _WIN32
    // Windows:TerminateProcess 根进程。会话级 Job 在 lubancode 退出时才兜底
    // 杀后代;用户主动停只杀根——多数后台命令是单进程,根死了就完了。根有
    // 子进程(npm start 起 node 之类)时后代可能逃逸,是已知局限,不在这层解。
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid)); h != nullptr) {
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
#else
    // POSIX:后台子进程 setsid() 了,pid 就是它自己的进程组 id。kill(-pid) 发
    // 给整组,连带子进程一起收(对齐 platform 层会话级收尾的 kill(-pid) 用法)。
    // 先 SIGTERM 客气一下,顽固的进程不管会再补 SIGKILL——但这里单次调用不
    // 等,先把 SIGTERM 发出去,实际收尾由 watcher 探活发现进程没了后标 Stopped。
    if (kill(-static_cast<pid_t>(pid), SIGTERM) != 0) {
        kill(static_cast<pid_t>(pid), SIGTERM);  // 进程组发不动就单发根
    }
#endif

    // 标终态:Stop 是用户/模型主动行为,退出码本来就不代表命令本身的成败,
    // 统一标 Stopped、exit_code=-1(被外部终止,非自然退出)。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : entries_) {
            if (entry->info.task_id == task_id && entry->info.status == BackgroundTaskStatus::Running) {
                entry->info.status = BackgroundTaskStatus::Stopped;
                entry->info.exit_code = -1;
                entry->info.finish_time = std::chrono::system_clock::now();
                entry->info.completed_reported = false;
                break;
            }
        }
    }
    return true;
}

std::vector<BackgroundTaskInfo> BackgroundTaskRegistry::DrainCompleted() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BackgroundTaskInfo> out;
    for (auto& entry : entries_) {
        if (entry->info.status != BackgroundTaskStatus::Running && !entry->info.completed_reported) {
            entry->info.completed_reported = true;
            out.push_back(entry->info);
        }
    }
    return out;
}

void BackgroundTaskRegistry::WatchThread(std::string task_id) {
    unsigned long pid = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : entries_) {
            if (entry->info.task_id == task_id) {
                pid = entry->info.pid;
                break;
            }
        }
    }
    if (pid == 0) {
        return;  // 防御:登记后表里居然找不到自己,不跑
    }

    // 轮询探活。stop_all_ 在析构时置位,这一拍检测到就立刻退出(不会超过
    // kPollIntervalMs 的延迟)。
    while (!stop_all_.load()) {
        int exit_code = 0;
        if (!IsPidAlive(pid, exit_code)) {
            // 进程结束了。标终态——再次确认表里这条还在 Running(可能 Stop()
            // 已经先一步把它标成 Stopped 了,那就别覆盖)。
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& entry : entries_) {
                if (entry->info.task_id == task_id && entry->info.status == BackgroundTaskStatus::Running) {
                    entry->info.exit_code = exit_code;
                    entry->info.status = (exit_code == 0) ? BackgroundTaskStatus::Completed
                                                          : BackgroundTaskStatus::Failed;
                    entry->info.finish_time = std::chrono::system_clock::now();
                    entry->info.completed_reported = false;
                    break;
                }
            }
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
}

bool BackgroundTaskRegistry::IsPidAlive(unsigned long pid, int& exit_code_out) {
#ifdef _WIN32
    // OpenProcess 拿查询句柄;GetExitCodeProcess 在进程还活着时返回 STILL_ACTIVE
    // (259),结束返回的就是退出码(精确)。句柄打不开(进程已死且 pid 已被
    // 回收、权限不够)当成"已结束"——这种情况下真实退出码已经拿不到了
    // (进程对象没了),乐观标 0(Completed):分不清成败时按成功算,模型读
    // 输出自己判断,跟 POSIX 脱离进程那条路语义对齐。
    const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) {
        exit_code_out = 0;
        return false;
    }
    DWORD code = 0;
    const bool ok = GetExitCodeProcess(h, &code) != 0;
    CloseHandle(h);
    if (!ok) {
        exit_code_out = 0;
        return false;
    }
    if (code == STILL_ACTIVE) {
        return true;
    }
    exit_code_out = static_cast<int>(code);
    return false;
#else
    // kill(pid, 0):0 信号不真发,只探测 pid 存不存在/能不能给它发信号。
    // 返回 0 = 活着;ESRCH = pid 不存在(进程结束了);EPERM = 存在但不归
    // 当前用户管(极少见,当活着等它自己了结)。脱离会话的后台进程退出码
    // 这里拿不到(不是当前进程的子进程,waitpid 不认)——分不清是真成功还是
    // 失败退出,乐观按 0(Completed)算:status 不误报成 Failed,模型读输出
    // 自己判断命令到底成没成。比起"分不清就当失败",对用户更友好。
    if (kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    exit_code_out = 0;
    return false;
#endif
}

}  // namespace lubancode::tools
