#include "tools/background_tasks.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "platform/text_encoding.hpp"
#include "tools/path_utils.hpp"

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

// 退化探活(watcher 没有 handle 时的兜底)轮询间隔。
constexpr int kPollIntervalMs = 200;

// ReadOutput 一次读多少字节上限。日志文件可能被长命进程写得很大,读全文
// 不限会吃光内存;超过就只取末尾这一段(尾部正是最新输出,最该看的部分)。
constexpr std::streamoff kReadCapBytes = 64 * 1024;

// 单任务日志的磁盘上限(进程生命线单 P1"日志可占满磁盘"):到顶截断保留
// 尾部一段(最新输出最该看),文件里追加一行标记说清"已到上限、前部已丢"。
// 策略三选一(轮转/停捕获/终止任务),这里选"截断保留尾部":不杀用户的
// dev server,也不放任磁盘被一条命令写满。watcher 每拍顺手查一眼。
constexpr std::uintmax_t kPerTaskLogCapBytes = 8 * 1024 * 1024;

// 终态任务保留上限(进程生命线单 P1"任务表与线程对象只增不减"):最多留
// 这么多条终态账;超出的最老一条删日志文件、出表。运行中任务永不淘汰。
constexpr std::size_t kTerminalRetention = 200;

}  // namespace

const char* BackgroundTaskStatusLabel(BackgroundTaskStatus s) {
    switch (s) {
        case BackgroundTaskStatus::Running: return "运行中";
        case BackgroundTaskStatus::Stopping: return "停止中";
        case BackgroundTaskStatus::Completed: return "完成(退出码 0)";
        case BackgroundTaskStatus::Failed: return "失败(非零或未知退出码)";
        case BackgroundTaskStatus::Stopped: return "已停止";
        case BackgroundTaskStatus::StopFailed: return "停止失败(进程可能还活着)";
    }
    return "未知";
}

BackgroundTaskRegistry& BackgroundTaskRegistry::Instance() {
    // magic static:线程安全初始化,第一次调用才构造,程序退出时自动析构
    // (析构里 stop_all_ 置位 + join 所有 watcher,把后台监听线程收干净)。
    static BackgroundTaskRegistry instance;
    return instance;
}

BackgroundTaskRegistry::~BackgroundTaskRegistry() {
    stop_all_.store(true);
    for (auto& entry : entries_) {
        if (entry->watcher.joinable()) {
            entry->watcher.join();
        }
    }
}

std::string BackgroundTaskRegistry::Register(std::string command, std::string shell, unsigned long pid,
                                              std::string log_path,
                                              std::shared_ptr<platform::BackgroundProcessHandle> handle,
                                              long long max_runtime_ms) {
    // P0 次序修复:先在锁内建 entry、分配 task_id、放进表,再起 watcher。
    // watcher 持 TaskState/handle 的共享指针,不回表里按 task_id 找自己——
    // 线程构造慢一点也无所谓,状态对象早就就位了。
    auto state = std::make_shared<TaskState>();
    auto entry = std::make_unique<Entry>();
    entry->info.task_id = std::to_string(next_id_.fetch_add(1));
    entry->info.command = std::move(command);
    entry->info.shell = std::move(shell);
    entry->info.pid = pid;
    entry->info.log_path = std::move(log_path);
    entry->info.status = BackgroundTaskStatus::Running;
    entry->info.encoding_hint = handle ? handle->encoding_hint : std::string();
    entry->info.start_time = std::chrono::system_clock::now();
    entry->state = state;
    entry->handle = std::move(handle);
    state->log_path = entry->info.log_path;

    const std::string task_id = entry->info.task_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(std::move(entry));
    }

    // 表里就位了才起 watcher;起失败(std::thread 构造抛)要回滚 entry,
    // 不能留一条没人照看的 Running(P0 方案里的硬要求)。
    auto* stored = [&]() -> Entry* {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& e : entries_) {
            if (e->info.task_id == task_id) {
                return e.get();
            }
        }
        return nullptr;
    }();
    if (stored == nullptr) {
        return task_id;  // 防御,不该发生
    }
    try {
        stored->watcher = std::thread([this, state, handle = stored->handle, pid, max_runtime_ms] {
            WatchThread(state, handle, pid, max_runtime_ms);
        });
    } catch (...) {
        // 回滚:entry 出表。任务进程还挂着(会话级收尾兜底),但台账不留
        // 一条永远 Running 的死账。
        std::lock_guard<std::mutex> lock(mutex_);
        std::erase_if(entries_, [&](const std::unique_ptr<Entry>& e) { return e->info.task_id == task_id; });
    }
    return task_id;
}

std::vector<BackgroundTaskInfo> BackgroundTaskRegistry::List() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BackgroundTaskInfo> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        BackgroundTaskInfo info = entry->info;
        // 状态从共享 TaskState 拿最新值(watcher 随时在写)。
        {
            std::lock_guard<std::mutex> slock(entry->state->mutex);
            info.status = entry->state->status;
            info.exit = entry->state->exit;
            info.finish_time = entry->state->finish_time;
        }
        out.push_back(std::move(info));
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
            BackgroundTaskInfo info = entry->info;
            {
                std::lock_guard<std::mutex> slock(entry->state->mutex);
                info.status = entry->state->status;
                info.exit = entry->state->exit;
                info.finish_time = entry->state->finish_time;
            }
            return info;
        }
    }
    return std::nullopt;
}

std::string BackgroundTaskRegistry::ReadOutput(const std::string& task_id, int tail_lines) {
    std::string log_path;
    std::string encoding_hint;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : entries_) {
            if (entry->info.task_id == task_id) {
                log_path = entry->info.log_path;
                encoding_hint = entry->info.encoding_hint;
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

    // 64KB 起刀处先退到完整换行:不把半行冒充完整行(首段若被截,后面加
    // 标记)。再退到 UTF-8 边界(清洗之后不会再有半字)。
    bool head_omitted = false;
    if (read_from > 0) {
        const auto first_nl = data.find('\n');
        if (first_nl != std::string::npos && first_nl + 1 < data.size()) {
            data = data.substr(first_nl + 1);
            head_omitted = true;
        } else if (first_nl != std::string::npos) {
            data.clear();
            head_omitted = true;
        }
    }

    // 编码出口(后台日志单):日志按原始字节落盘,这里按 hint 清洗。
    //   utf-8(powershell wrapper)      -> SanitizeUtf8(已合法则原样)
    //   oem-ansi(cmd)/unknown/空 hint  -> SanitizeUtf8:先验 UTF-8,非法时
    //      Windows 上按 ACP 重解(cmd 的 OEM/ANSI 字节那条路),再不行
    //      逐段替换 U+FFFD;非 Windows 直接逐段替换。不无声猜代码页。
    data = platform::SanitizeUtf8(data);

    std::string prefix;
    if (head_omitted) {
        prefix = "[日志前部已省略]\n";
    }

    if (tail_lines <= 0) {
        return prefix + data;  // 全文(已截断到末尾 64KB)
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
    std::ostringstream oss;
    oss << prefix;
    if (static_cast<int>(lines.size()) <= tail_lines) {
        for (const auto& l : lines) {
            oss << l;
        }
        return oss.str();
    }
    const std::size_t start = lines.size() - static_cast<std::size_t>(tail_lines);
    for (std::size_t i = start; i < lines.size(); ++i) {
        oss << lines[i];
    }
    return oss.str();
}

bool BackgroundTaskRegistry::Stop(const std::string& task_id) {
    std::shared_ptr<TaskState> state;
    std::shared_ptr<platform::BackgroundProcessHandle> handle;
    unsigned long pid = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : entries_) {
            if (entry->info.task_id == task_id) {
                std::lock_guard<std::mutex> slock(entry->state->mutex);
                if (entry->state->status != BackgroundTaskStatus::Running &&
                    entry->state->status != BackgroundTaskStatus::Stopping &&
                    entry->state->status != BackgroundTaskStatus::StopFailed) {
                    return true;  // 已终态,不重复杀
                }
                state = entry->state;
                handle = entry->handle;
                pid = entry->info.pid;
                break;
            }
        }
    }
    if (state == nullptr) {
        return false;
    }

    // 有 handle:Stop 落 TerminateTree(整棵树)。状态先 Stopping,树死透了
    // 才 Stopped;收不动如实 StopFailed,不先盖章。
    if (handle != nullptr) {
        {
            std::lock_guard<std::mutex> slock(state->mutex);
            state->status = BackgroundTaskStatus::Stopping;
        }
        const bool killed = handle->TerminateTree(2000);
        std::lock_guard<std::mutex> slock(state->mutex);
        if (killed) {
            state->status = BackgroundTaskStatus::Stopped;
            const auto completion = handle->Peek();
            if (completion.known) {
                state->exit.exit_code = completion.exit_code;
                if (completion.signal != 0) {
                    state->exit.signal = completion.signal;
                }
            } else {
                state->exit.exit_code = std::nullopt;  // 不知道便是不知道
            }
        } else {
            state->status = BackgroundTaskStatus::StopFailed;
            state->exit.exit_code = std::nullopt;
        }
        state->finish_time = std::chrono::system_clock::now();
        state->completed_reported = false;
        return true;
    }

    // 退化老路(测试直接 Register 没给 handle):PID 杀,标 Stopped。
#ifdef _WIN32
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid)); h != nullptr) {
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
#else
    if (kill(-static_cast<pid_t>(pid), SIGTERM) != 0) {
        kill(static_cast<pid_t>(pid), SIGTERM);
    }
#endif
    {
        std::lock_guard<std::mutex> slock(state->mutex);
        state->status = BackgroundTaskStatus::Stopped;
        state->exit.exit_code = std::nullopt;
        state->finish_time = std::chrono::system_clock::now();
        state->completed_reported = false;
    }
    return true;
}

std::vector<BackgroundTaskInfo> BackgroundTaskRegistry::DrainCompleted() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BackgroundTaskInfo> out;
    for (auto& entry : entries_) {
        std::lock_guard<std::mutex> slock(entry->state->mutex);
        const bool terminal = entry->state->status != BackgroundTaskStatus::Running &&
                              entry->state->status != BackgroundTaskStatus::Stopping &&
                              entry->state->status != BackgroundTaskStatus::StopFailed;
        if (terminal && !entry->state->completed_reported) {
            entry->state->completed_reported = true;
            BackgroundTaskInfo info = entry->info;
            info.status = entry->state->status;
            info.exit = entry->state->exit;
            info.finish_time = entry->state->finish_time;
            out.push_back(std::move(info));
        }
    }
    return out;
}

void BackgroundTaskRegistry::WatchThread(std::shared_ptr<TaskState> state,
                                          std::shared_ptr<platform::BackgroundProcessHandle> handle,
                                          unsigned long pid, long long max_runtime_ms) {
    // 单任务日志截断(策略:到顶截断保留尾部 + 标记,不杀任务不轮转)。
    // log_path 在 Register 时就写进 state、之后不变, watcher 只读。
    const std::string log_path_copy = state->log_path;
    const auto cap_log = [&log_path_copy] {
        if (log_path_copy.empty()) {
            return;
        }
        std::error_code ec;
        const auto log_fs_path = Utf8ToPath(log_path_copy);
        const auto size = std::filesystem::file_size(log_fs_path, ec);
        if (ec || size <= kPerTaskLogCapBytes) {
            return;
        }
        // 截断:保留尾部 3/4(上限内),重写文件,追加标记行。进程还在写
        // 也没关系——truncate 后的追加写继续落在新尾部,不劈内容语义。
        std::ifstream in(log_fs_path, std::ios::binary | std::ios::ate);
        if (!in.is_open()) {
            return;
        }
        const auto keep = static_cast<std::streamoff>(kPerTaskLogCapBytes / 4 * 3);
        in.seekg(-keep, std::ios::end);
        std::string tail(static_cast<std::size_t>(keep), '\0');
        in.read(tail.data(), keep);
        tail.resize(static_cast<std::size_t>(in.gcount()));
        // 头一枚换行起刀,不把半行冒充完整行。
        const auto first_nl = tail.find('\n');
        if (first_nl != std::string::npos && first_nl + 1 < tail.size()) {
            tail = tail.substr(first_nl + 1);
        }
        std::ofstream out(log_fs_path, std::ios::binary | std::ios::trunc);
        out << "[日志超过单任务上限 8MB,已截断保留尾部]\n" << tail;
    };

    const auto started_at = std::chrono::steady_clock::now();
    const bool has_max_runtime = max_runtime_ms > 0;

    // 有 handle:等在原生句柄上(200ms 一片,好响应 stop_all_),退出码由
    // 唯一收尸方写进完成态——不拿 PID 猜,不把不知道涂成 0。
    if (handle != nullptr) {
        while (!stop_all_.load()) {
            if (handle->Wait(kPollIntervalMs)) {
                const auto completion = handle->Peek();
                {
                    std::lock_guard<std::mutex> slock(state->mutex);
                    if (state->status == BackgroundTaskStatus::Running) {
                        if (completion.known) {
                            state->exit.exit_code = completion.exit_code;
                            if (completion.signal != 0) {
                                state->exit.signal = completion.signal;
                            }
                            state->status = (completion.exit_code == 0 && completion.signal == 0)
                                                ? BackgroundTaskStatus::Completed
                                                : BackgroundTaskStatus::Failed;
                        } else {
                            // 收尸方没拿到状态:如实 Failed + nullopt,不借 0 冒充成功。
                            state->exit.exit_code = std::nullopt;
                            state->status = BackgroundTaskStatus::Failed;
                        }
                        state->finish_time = std::chrono::system_clock::now();
                        state->completed_reported = false;
                    }
                }
                // 锁序:PruneTerminalTasks 内部拿 mutex_ + 各 entry 的
                // state->mutex,与 List/Get(先 mutex_ 后 state->mutex)同
                // 序——但绝不能在本条 state->mutex 还攥着时调,否则和
                // 正在 List 的线程对锁死。出了上面的作用域再调。
                PruneTerminalTasks();
                return;
            }
            cap_log();
            // max_runtime_ms 到点收树(P2:不改 timeout_ms 旧义,另立的墙)。
            if (has_max_runtime) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - started_at)
                                         .count();
                if (elapsed >= max_runtime_ms && handle->TerminateTree(2000)) {
                    {
                        std::lock_guard<std::mutex> slock(state->mutex);
                        if (state->status == BackgroundTaskStatus::Running) {
                            state->status = BackgroundTaskStatus::Stopped;
                            const auto completion = handle->Peek();
                            state->exit.exit_code =
                                completion.known ? std::optional<int>(completion.exit_code) : std::nullopt;
                            state->finish_time = std::chrono::system_clock::now();
                            state->completed_reported = false;
                        }
                    }
                    PruneTerminalTasks();  // 同上:出了 state->mutex 作用域再调
                    return;
                }
            }
        }
        return;
    }

    // 退化老路(没有 handle):轮询 PID 探活。退出码拿不到就 nullopt。
    while (!stop_all_.load()) {
        if (!IsPidAlive(pid)) {
            std::lock_guard<std::mutex> slock(state->mutex);
            if (state->status == BackgroundTaskStatus::Running) {
                state->exit.exit_code = std::nullopt;
                state->status = BackgroundTaskStatus::Failed;  // 未知:不冒充 Completed
                state->finish_time = std::chrono::system_clock::now();
                state->completed_reported = false;
            }
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
}

// 终态任务保留上限:超出的最老终态条目删日志、出表。运行中任务永不淘汰;
// 被淘汰的条目如果还没被 DrainCompleted 取走,通知就没了——所以只淘汰
// completed_reported=true 的(取走通知之后的收尾动作)。
void BackgroundTaskRegistry::PruneTerminalTasks() {
    // 两段式:锁内只做"挑出要淘汰的条目并出表"(快);锁外 join watcher
    // 线程、删日志文件(慢,且 join 绝不能攥着 mutex_——被 join 的线程
    // 若正要进来抢 mutex_ 做自己的收尾,就互相等死)。
    std::vector<std::unique_ptr<Entry>> evicted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t terminal = 0;
        for (const auto& e : entries_) {
            std::lock_guard<std::mutex> slock(e->state->mutex);
            if (e->state->status != BackgroundTaskStatus::Running &&
                e->state->status != BackgroundTaskStatus::Stopping &&
                e->state->status != BackgroundTaskStatus::StopFailed) {
                ++terminal;
            }
        }
        if (terminal <= kTerminalRetention) {
            return;
        }
        // 终态里挑最老的 task_id(数字最小)淘汰,直到回到上限。只淘汰
        // completed_reported=true 的(通知已取走;没取走的留着,别吞通知)。
        std::size_t to_remove = terminal - kTerminalRetention;
        std::vector<std::unique_ptr<Entry>> kept;
        kept.reserve(entries_.size());
        // 先按 task_id 升序排一份视图,老的最先走。
        std::vector<Entry*> by_id;
        by_id.reserve(entries_.size());
        for (auto& e : entries_) {
            by_id.push_back(e.get());
        }
        std::sort(by_id.begin(), by_id.end(), [](const Entry* a, const Entry* b) {
            return std::stoull(a->info.task_id) < std::stoull(b->info.task_id);
        });
        for (Entry* candidate : by_id) {
            if (to_remove == 0) {
                break;
            }
            std::lock_guard<std::mutex> slock(candidate->state->mutex);
            const bool terminal_state = candidate->state->status != BackgroundTaskStatus::Running &&
                                        candidate->state->status != BackgroundTaskStatus::Stopping &&
                                        candidate->state->status != BackgroundTaskStatus::StopFailed;
            if (terminal_state && candidate->state->completed_reported) {
                evicted.emplace_back();
                // 从 entries_ 里找到这个指针,移走所有权。
                for (auto& e : entries_) {
                    if (e.get() == candidate) {
                        evicted.back() = std::move(e);
                        break;
                    }
                }
                --to_remove;
            }
        }
        std::erase_if(entries_, [](const std::unique_ptr<Entry>& e) { return e == nullptr; });
    }
    // 锁外收尾:join(被淘汰条目的 watcher 都已 return,join 即回)再删日志。
    for (auto& entry : evicted) {
        if (entry->watcher.joinable()) {
            entry->watcher.join();
        }
        std::error_code ec;
        std::filesystem::remove(Utf8ToPath(entry->info.log_path), ec);
    }
}

bool BackgroundTaskRegistry::IsPidAlive(unsigned long pid) {
#ifdef _WIN32
    const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) {
        return false;
    }
    DWORD code = 0;
    const bool ok = GetExitCodeProcess(h, &code) != 0;
    CloseHandle(h);
    if (!ok) {
        return false;
    }
    return code == STILL_ACTIVE;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

}  // namespace lubancode::tools
