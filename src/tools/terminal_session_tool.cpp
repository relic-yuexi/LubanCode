#include "tools/terminal_session_tool.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "tools/terminal_keys.hpp"
#include "tools/terminal_screen.hpp"
#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

#ifndef _WIN32
#include <unistd.h>  // access(X_OK):找 bash

#include <filesystem>
#include <system_error>

#include "platform/pty.hpp"
#include "tools/isolation.hpp"
#include "tools/path_utils.hpp"
#endif

namespace lubancode::tools {

std::string TerminalSessionTool::name() const {
    return "terminal";
}

std::string TerminalSessionTool::description() const {
    // 文案在 src/prompts/tools/<语言>/terminal.md,兜底是这段原文。
    return ToolText("terminal", "description",
                    "交互终端会话(PTY,仅 POSIX):驱动 vim、python/sqlite REPL、交互式安装向导、ssh、gdb "
                    "这类要键盘交互的程序。action=open 起 bash 会话,send 送按键(<Enter> <Tab> <Esc> <BS> "
                    "<Up>/<Down>/<Left>/<Right> <C-c> <C-d> <C-z> 等记号,字面 < 写 <lt>),read 看当前屏幕,"
                    "close 结束会话,list 列会话。普通一次性命令仍用 run_command。");
}

nlohmann::json TerminalSessionTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";
    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json action = nlohmann::json::object();
    action["type"] = "string";
    action["enum"] = nlohmann::json::array({"open", "send", "read", "close", "list"});
    action["description"] = ToolText("terminal", "param.action",
                                     "open 起会话 / send 送按键 / read 看屏 / close 结束 / list 列会话。");
    properties["action"] = action;

    nlohmann::json session_id = nlohmann::json::object();
    session_id["type"] = "integer";
    session_id["description"] = ToolText("terminal", "param.session_id", "会话编号(open 返回的);send/read/close 必填。");
    properties["session_id"] = session_id;

    nlohmann::json keys = nlohmann::json::object();
    keys["type"] = "string";
    keys["description"] = ToolText("terminal", "param.keys",
                                   "send 要送的按键文本,可夹尖括号记号:<Enter> <Tab> <Esc> <BS> <Space> "
                                   "<Up> <Down> <Left> <Right> <Home> <End> <PageUp> <PageDown> <Delete> "
                                   "<F1>..<F12> <C-a>..<C-z> <M-x>;字面 < 写 <lt>。");
    properties["keys"] = keys;

    nlohmann::json literal = nlohmann::json::object();
    literal["type"] = "boolean";
    literal["description"] = ToolText("terminal", "param.literal", "为 true 时 keys 原样发送,不解析尖括号记号。");
    properties["literal"] = literal;

    nlohmann::json wait_ms = nlohmann::json::object();
    wait_ms["type"] = "integer";
    wait_ms["description"] = ToolText("terminal", "param.wait_ms",
                                      "送完/查看前最多等多少毫秒:有新输出且静默 300ms 即提前返回。"
                                      "send/open 默认 1000,read 默认 0,上限 60000。");
    properties["wait_ms"] = wait_ms;

    nlohmann::json cwd = nlohmann::json::object();
    cwd["type"] = "string";
    cwd["description"] = ToolText("terminal", "param.cwd", "open 的工作目录,缺省为当前工作目录。");
    properties["cwd"] = cwd;

    nlohmann::json rows = nlohmann::json::object();
    rows["type"] = "integer";
    rows["description"] = ToolText("terminal", "param.rows", "open 的屏幕行数,默认 40(5..100)。");
    properties["rows"] = rows;

    nlohmann::json cols = nlohmann::json::object();
    cols["type"] = "integer";
    cols["description"] = ToolText("terminal", "param.cols", "open 的屏幕列数,默认 160(20..400)。");
    properties["cols"] = cols;

    nlohmann::json history = nlohmann::json::object();
    history["type"] = "integer";
    history["description"] = ToolText("terminal", "param.history_lines",
                                      "额外附上主屏滚出顶端的最近多少行历史,默认 0(上限 2000)。");
    properties["history_lines"] = history;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"action"});
    return schema;
}

Tool::Result TerminalSessionTool::execute(const nlohmann::json& input) {
    return Run(input, nullptr);
}

Tool::Result TerminalSessionTool::execute(const nlohmann::json& input, const ToolExecutionContext& context) {
    return Run(input, context.cancel);
}

#ifdef _WIN32

Tool::Result TerminalSessionTool::Run(const nlohmann::json& input, const std::atomic<bool>* cancel) {
    (void)input;
    (void)cancel;
    return Result::Error("terminal 工具仅支持 POSIX(Linux/macOS);Windows 上请用 run_command。");
}

void CloseAllTerminalSessions() {}

#else  // POSIX

namespace {

constexpr long long kDefaultRows = 40;
constexpr long long kDefaultCols = 160;
constexpr long long kMinRows = 5;
constexpr long long kMaxRows = 100;
constexpr long long kMinCols = 20;
constexpr long long kMaxCols = 400;
constexpr long long kDefaultSendWaitMs = 1000;
constexpr long long kMaxWaitMs = 60000;
constexpr long long kMaxHistoryLines = 2000;
constexpr std::size_t kMaxKeysBytes = 64 * 1024;
constexpr int kQuietMs = 300;  // 有输出后静默这么久即视为"这一拨出完了"

// 整数参数:缺省/null 不动 out;类型不对填 error 返回 false。
bool ReadIntParam(const nlohmann::json& input, const char* key, long long& out, std::string& error) {
    const auto it = input.find(key);
    if (it == input.end() || it->is_null()) {
        return true;
    }
    if (!it->is_number_integer()) {
        error = std::string(key) + " 得是整数";
        return false;
    }
    out = it->get<long long>();
    return true;
}

using Clock = std::chrono::steady_clock;

struct Session {
    Session(int r, int c) : rows(r), cols(c), screen(r, c) {}
    ~Session() {
        // 兜底:正常路径 Terminate 已 join、已关主端;这里防异常路径漏收。
        stop.store(true);
        if (reader.joinable()) {
            reader.join();
        }
        platform::PtyCloseMaster(master_fd);
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    int id = 0;
    int pid = -1;
    int master_fd = -1;
    std::string shell;
    std::string cwd;
    int rows;
    int cols;

    std::mutex mu;
    std::condition_variable cv;
    // 写主端与关主端互斥:send 的写可能等上几秒(程序不读输入),不能占着
    // mu 卡住读线程;另用一把锁,只保证不往已关(fd 号可能被复用)的主端写。
    std::mutex write_mu;
    TerminalScreen screen;            // mu 保护
    std::uint64_t output_seq = 0;     // mu 保护:每读到一段输出 +1
    Clock::time_point last_output{};  // mu 保护
    bool eof = false;                 // mu 保护:从端全关,不会再有输出
    bool exited = false;              // mu 保护:子进程已收尸
    platform::PtyExitStatus exit_status;  // mu 保护

    std::atomic<bool> stop{false};
    std::thread reader;
};

// 读线程:非阻塞读主端喂仿真器,把仿真器的应答(DSR/DA)写回,顺带探子进程退出。
void ReaderLoop(Session* s) {
    std::array<char, 8192> buffer{};
    std::string pending_replies;  // 待写回的应答:写锁被 send 占着时攒到下一拍
    while (!s->stop.load()) {
        if (!pending_replies.empty()) {
            // try_lock:send 正卡在大段写入时,读线程不能跟着等——它停了抽
            // 输出,程序写不动、就不读输入,send 的写也永远写不完。
            std::unique_lock<std::mutex> write_lock(s->write_mu, std::try_to_lock);
            if (write_lock.owns_lock()) {
                platform::PtyWriteAll(s->master_fd, pending_replies, 200);
                pending_replies.clear();
            }
        }
        bool eof_now = false;
        {
            std::lock_guard<std::mutex> lock(s->mu);
            eof_now = s->eof;
        }
        if (!eof_now) {
            const int n = platform::PtyRead(s->master_fd, buffer.data(), buffer.size(), 100);
            if (n > 0) {
                std::string replies;
                {
                    std::lock_guard<std::mutex> lock(s->mu);
                    s->screen.Feed(std::string_view(buffer.data(), static_cast<std::size_t>(n)));
                    replies = s->screen.TakeReplies();
                    ++s->output_seq;
                    s->last_output = Clock::now();
                }
                pending_replies += replies;
                s->cv.notify_all();
            } else if (n < 0) {
                {
                    std::lock_guard<std::mutex> lock(s->mu);
                    s->eof = true;
                }
                s->cv.notify_all();
            }
        } else {
            // 从端关了但子进程还没收尸(少见):别空转。
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        bool exited_now = false;
        {
            std::lock_guard<std::mutex> lock(s->mu);
            exited_now = s->exited;
        }
        if (!exited_now) {
            const platform::PtyExitStatus status = platform::PtyPollExit(s->pid);
            if (status.exited) {
                {
                    std::lock_guard<std::mutex> lock(s->mu);
                    s->exited = true;
                    s->exit_status = status;
                }
                s->cv.notify_all();
            }
        }
        {
            std::lock_guard<std::mutex> lock(s->mu);
            if (s->eof && s->exited) {
                break;
            }
        }
    }
}

// 等输出落定:有新输出且静默 kQuietMs、或到时、或进程退净、或取消,先到为准。
void WaitSettled(Session& s, long long wait_ms, std::uint64_t start_seq, const std::atomic<bool>* cancel) {
    if (wait_ms <= 0) {
        return;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
    const auto quiet = std::chrono::milliseconds(std::min<long long>(kQuietMs, wait_ms));
    std::unique_lock<std::mutex> lock(s.mu);
    for (;;) {
        if (cancel != nullptr && cancel->load()) {
            return;
        }
        if (s.exited && s.eof) {
            return;
        }
        const auto now = Clock::now();
        if (now >= deadline) {
            return;
        }
        if (s.output_seq != start_seq && now - s.last_output >= quiet) {
            return;
        }
        const auto step = std::min<Clock::duration>(deadline - now, std::chrono::milliseconds(50));
        s.cv.wait_for(lock, step);
    }
}

bool WaitExited(Session& s, int ms) {
    std::unique_lock<std::mutex> lock(s.mu);
    return s.cv.wait_for(lock, std::chrono::milliseconds(ms), [&s] { return s.exited; });
}

bool IsExited(Session& s) {
    std::lock_guard<std::mutex> lock(s.mu);
    return s.exited;
}

void StopReader(Session& s) {
    s.stop.store(true);
    if (s.reader.joinable()) {
        s.reader.join();
    }
    {
        std::lock_guard<std::mutex> write_lock(s.write_mu);
        platform::PtyCloseMaster(s.master_fd);
        s.master_fd = -1;
    }
    if (!IsExited(s)) {
        const platform::PtyExitStatus status = platform::PtyPollExit(s.pid);
        std::lock_guard<std::mutex> lock(s.mu);
        if (status.exited) {
            s.exited = true;
            s.exit_status = status;
        }
    }
}

// 收一只会话:SIGHUP -> SIGTERM -> SIGKILL,每档等子进程退出;再停读线程、关主端。
void Terminate(Session& s) {
    if (!IsExited(s)) {
        platform::PtySignalGroups(s.pid, s.master_fd, platform::PtySigHup());
        if (!WaitExited(s, 500)) {
            platform::PtySignalGroups(s.pid, s.master_fd, platform::PtySigTerm());
            if (!WaitExited(s, 1000)) {
                platform::PtySignalGroups(s.pid, s.master_fd, platform::PtySigKill());
                WaitExited(s, 1000);
            }
        }
    }
    StopReader(s);
}

std::string StatusLabel(const Session& s) {
    if (!s.exited) {
        return "运行中";
    }
    if (s.exit_status.exit_code.has_value()) {
        return "已退出(退出码 " + std::to_string(*s.exit_status.exit_code) + ")";
    }
    if (s.exit_status.signal.has_value()) {
        return "已退出(被信号 " + std::to_string(*s.exit_status.signal) + " 终止)";
    }
    return "已退出(退出码未知)";
}

// 渲染快照文本 + 结构化 details。
Tool::Result Render(Session& s, long long history_lines, const std::string& note, bool is_error = false) {
    std::lock_guard<std::mutex> lock(s.mu);
    std::ostringstream oss;
    if (!note.empty()) {
        oss << note << "\n";
    }
    oss << "[terminal 会话 #" << s.id << "] pid=" << s.pid << " · " << StatusLabel(s) << " · " << s.rows << "x"
        << s.cols << " · 光标 行" << (s.screen.cursor_row() + 1) << " 列" << (s.screen.cursor_col() + 1);
    if (s.screen.alternate_screen()) {
        oss << " · 备用屏(全屏程序)";
    }
    oss << "\n";
    if (history_lines > 0) {
        const auto history = s.screen.Scrollback(static_cast<std::size_t>(history_lines));
        oss << "---- 滚动历史(末 " << history.size() << " 行) ----\n";
        for (const auto& line : history) {
            oss << line << "\n";
        }
    }
    oss << "---- 屏幕 ----\n" << s.screen.Snapshot() << "\n---- 屏幕完 ----";
    if (s.exited) {
        oss << "\n会话进程已退出;用 action=close 释放名额。";
    }

    Tool::Result result(oss.str(), is_error);
    result.details["session_id"] = s.id;
    result.details["pid"] = s.pid;
    result.details["running"] = !s.exited;
    if (s.exit_status.exit_code.has_value()) {
        result.details["exit_code"] = *s.exit_status.exit_code;
    }
    if (s.exit_status.signal.has_value()) {
        result.details["signal"] = *s.exit_status.signal;
    }
    result.details["cursor_row"] = s.screen.cursor_row();
    result.details["cursor_col"] = s.screen.cursor_col();
    result.details["alternate_screen"] = s.screen.alternate_screen();
    return result;
}

// bash 优先(交互体验与作业控制),找不到退 /bin/sh。fork 前解析成绝对路径。
std::string ResolveShell() {
    for (const char* candidate : {"/bin/bash", "/usr/bin/bash", "/usr/local/bin/bash", "/opt/homebrew/bin/bash"}) {
        if (::access(candidate, X_OK) == 0) {
            return candidate;
        }
    }
    if (const char* path = std::getenv("PATH"); path != nullptr) {
        std::string_view rest(path);
        while (!rest.empty()) {
            const std::size_t colon = rest.find(':');
            const std::string_view dir = rest.substr(0, colon);
            if (!dir.empty()) {
                const std::string full = std::string(dir) + "/bash";
                if (::access(full.c_str(), X_OK) == 0) {
                    return full;
                }
            }
            if (colon == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(colon + 1);
        }
    }
    return "/bin/sh";
}

class Manager {
public:
    static Manager& Instance() {
        // magic static:首用构造,进程退出时析构收掉全部会话(读线程 join 干净)。
        static Manager instance;
        return instance;
    }
    ~Manager() { CloseAll(); }

    std::shared_ptr<Session> Find(int id) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = sessions_.find(id);
        return it == sessions_.end() ? nullptr : it->second;
    }

    std::vector<std::shared_ptr<Session>> All() {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<std::shared_ptr<Session>> out;
        for (const auto& entry : sessions_) {
            out.push_back(entry.second);
        }
        return out;
    }

    std::shared_ptr<Session> Remove(int id) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            return nullptr;
        }
        auto session = it->second;
        sessions_.erase(it);
        return session;
    }

    // 占名额:满了先挤掉最老的已退出会话(返回给调用方在锁外收尸);仍满返回 false。
    bool Reserve(std::vector<std::shared_ptr<Session>>& evicted) {
        std::lock_guard<std::mutex> lock(mu_);
        while (sessions_.size() + reserved_ >= kMaxTerminalSessions) {
            auto victim = sessions_.end();
            for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
                if (IsExited(*it->second)) {
                    victim = it;
                    break;  // map 按 id 升序:第一只即最老
                }
            }
            if (victim == sessions_.end()) {
                return false;
            }
            evicted.push_back(victim->second);
            sessions_.erase(victim);
        }
        ++reserved_;
        return true;
    }

    void Unreserve() {
        std::lock_guard<std::mutex> lock(mu_);
        --reserved_;
    }

    int Commit(const std::shared_ptr<Session>& session) {
        std::lock_guard<std::mutex> lock(mu_);
        --reserved_;
        session->id = next_id_++;
        sessions_[session->id] = session;
        return session->id;
    }

    // 退出收尾:分档并行收(先全体 SIGHUP,共等 300ms,剩下的 SIGKILL),
    // 不逐只串行等,免得退出时拖上好几秒。
    void CloseAll() {
        std::vector<std::shared_ptr<Session>> all;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& entry : sessions_) {
                all.push_back(std::move(entry.second));
            }
            sessions_.clear();
        }
        if (all.empty()) {
            return;
        }
        for (auto& s : all) {
            if (!IsExited(*s)) {
                platform::PtySignalGroups(s->pid, s->master_fd, platform::PtySigHup());
            }
        }
        const auto wait_all = [&all](int total_ms) {
            const auto deadline = Clock::now() + std::chrono::milliseconds(total_ms);
            for (auto& s : all) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
                if (left.count() <= 0) {
                    break;
                }
                WaitExited(*s, static_cast<int>(left.count()));
            }
        };
        wait_all(300);
        for (auto& s : all) {
            if (!IsExited(*s)) {
                platform::PtySignalGroups(s->pid, s->master_fd, platform::PtySigKill());
            }
        }
        wait_all(500);
        for (auto& s : all) {
            StopReader(*s);
        }
    }

private:
    Manager() = default;

    std::mutex mu_;
    std::map<int, std::shared_ptr<Session>> sessions_;
    std::size_t reserved_ = 0;
    int next_id_ = 1;
};

std::optional<int> ReadSessionId(const nlohmann::json& input, std::string& error) {
    const auto it = input.find("session_id");
    if (it == input.end() || it->is_null()) {
        error = "缺 session_id(open 返回的会话编号)";
        return std::nullopt;
    }
    if (it->is_number_integer()) {
        return static_cast<int>(it->get<long long>());
    }
    if (it->is_string()) {
        const std::string text = it->get<std::string>();
        char* end = nullptr;
        const long value = std::strtol(text.c_str(), &end, 10);
        if (!text.empty() && end != nullptr && *end == '\0') {
            return static_cast<int>(value);
        }
    }
    error = "session_id 得是整数";
    return std::nullopt;
}

Tool::Result NoSuchSession(int id) {
    std::ostringstream oss;
    oss << "没有编号为 " << id << " 的 terminal 会话(可能已 close)。";
    const auto all = Manager::Instance().All();
    if (all.empty()) {
        oss << "当前没有会话,先 action=open。";
    } else {
        oss << "在册:";
        for (const auto& s : all) {
            oss << " #" << s->id;
        }
    }
    return Tool::Result::Error(oss.str());
}

Tool::Result DoOpen(const nlohmann::json& input, const std::atomic<bool>* cancel) {
    std::string error;
    long long rows = kDefaultRows;
    long long cols = kDefaultCols;
    long long wait_ms = kDefaultSendWaitMs;
    if (!ReadIntParam(input, "rows", rows, error) || !ReadIntParam(input, "cols", cols, error) ||
        !ReadIntParam(input, "wait_ms", wait_ms, error)) {
        return Tool::Result::Error(error);
    }
    rows = std::clamp(rows, kMinRows, kMaxRows);
    cols = std::clamp(cols, kMinCols, kMaxCols);
    wait_ms = std::clamp(wait_ms, 0LL, kMaxWaitMs);

    // cwd:与 run_command 同口径——相对路径按隔离房或当前目录拼,须是真目录,
    // 住在 worktree 房里的会话不许指回主 checkout。
    std::string cwd;
    if (const auto it = input.find("cwd"); it != input.end() && !it->is_null()) {
        if (!it->is_string()) {
            return Tool::Result::Error("cwd 得是字符串(目录路径)");
        }
        cwd = it->get<std::string>();
        if (cwd.find('\0') != std::string::npos) {
            return Tool::Result::Error("cwd 里带 NUL 字符,拒绝执行");
        }
    }
    const IsolationScope* isolation = IsolationGuard::Current();
    {
        std::error_code ec;
        std::filesystem::path base =
            isolation != nullptr ? Utf8ToPath(isolation->base_dir) : std::filesystem::current_path(ec);
        std::filesystem::path cwd_path = cwd.empty() ? base : Utf8ToPath(cwd);
        if (cwd_path.is_relative()) {
            cwd_path = base / cwd_path;
        }
        if (!std::filesystem::is_directory(cwd_path, ec)) {
            return Tool::Result::Error("cwd 不是真实存在的目录: " + (cwd.empty() ? PathToUtf8(cwd_path) : cwd));
        }
        cwd = PathToUtf8(cwd_path);
    }
    if (isolation != nullptr && PathBlockedByIsolation(cwd, *isolation)) {
        return Tool::Result::Error("[隔离] 终端工作目录指回主 checkout,已拦: " + cwd + "(会话住在 worktree " +
                                   isolation->name + " 里)。请在房内开终端。");
    }

    auto& manager = Manager::Instance();
    std::vector<std::shared_ptr<Session>> evicted;
    const bool reserved = manager.Reserve(evicted);
    for (auto& old : evicted) {
        Terminate(*old);
    }
    if (!reserved) {
        return Tool::Result::Error("terminal 会话已满(最多 " + std::to_string(kMaxTerminalSessions) +
                                   " 个同时在跑),先 action=close 关掉不用的。");
    }

    platform::PtySpawnOptions options;
    options.program = ResolveShell();
    const std::size_t slash = options.program.find_last_of('/');
    options.argv = {slash == std::string::npos ? options.program : options.program.substr(slash + 1)};
    options.cwd = cwd;
    options.rows = static_cast<int>(rows);
    options.cols = static_cast<int>(cols);
    auto spawned = platform::SpawnPty(options);
    if (!spawned.has_value()) {
        manager.Unreserve();
        return Tool::Result::Error("起终端会话失败: " + spawned.error());
    }

    auto session = std::make_shared<Session>(static_cast<int>(rows), static_cast<int>(cols));
    session->pid = spawned->pid;
    session->master_fd = spawned->master_fd;
    session->shell = options.program;
    session->cwd = cwd;
    try {
        session->reader = std::thread(ReaderLoop, session.get());
    } catch (...) {
        manager.Unreserve();
        Terminate(*session);
        return Tool::Result::Error("起终端会话失败: 读线程创建失败");
    }
    manager.Commit(session);

    WaitSettled(*session, wait_ms, 0, cancel);
    return Render(*session, 0, "已开终端会话(" + session->shell + ",cwd " + cwd + ")。");
}

Tool::Result DoSend(const nlohmann::json& input, const std::atomic<bool>* cancel) {
    std::string error;
    const auto id = ReadSessionId(input, error);
    if (!id.has_value()) {
        return Tool::Result::Error(error);
    }
    const auto keys_it = input.find("keys");
    if (keys_it == input.end() || !keys_it->is_string()) {
        return Tool::Result::Error("send 需要 keys(字符串)");
    }
    const std::string keys = keys_it->get<std::string>();
    if (keys.size() > kMaxKeysBytes) {
        return Tool::Result::Error("keys 太长(上限 64KB);大段内容请先 write_file 落盘再在终端里读入");
    }
    bool literal = false;
    if (const auto it = input.find("literal"); it != input.end() && !it->is_null()) {
        if (!it->is_boolean()) {
            return Tool::Result::Error("literal 得是布尔值");
        }
        literal = it->get<bool>();
    }
    long long wait_ms = kDefaultSendWaitMs;
    long long history = 0;
    if (!ReadIntParam(input, "wait_ms", wait_ms, error) || !ReadIntParam(input, "history_lines", history, error)) {
        return Tool::Result::Error(error);
    }
    wait_ms = std::clamp(wait_ms, 0LL, kMaxWaitMs);
    history = std::clamp(history, 0LL, kMaxHistoryLines);

    auto session = Manager::Instance().Find(*id);
    if (session == nullptr) {
        return NoSuchSession(*id);
    }
    bool app_cursor = false;
    bool dead = false;
    std::uint64_t start_seq = 0;
    {
        std::lock_guard<std::mutex> lock(session->mu);
        app_cursor = session->screen.app_cursor_keys();
        dead = session->exited || session->eof;
        start_seq = session->output_seq;
    }
    if (dead) {
        return Render(*session, history, "会话进程已退出,按键没送出去。", true);
    }
    std::string bytes;
    if (literal) {
        bytes = keys;
    } else {
        auto parsed = ParseTerminalKeys(keys, app_cursor);
        if (!parsed.has_value()) {
            return Tool::Result::Error(parsed.error());
        }
        bytes = std::move(*parsed);
    }
    {
        std::lock_guard<std::mutex> write_lock(session->write_mu);
        if (session->master_fd < 0) {
            return Tool::Result::Error("会话 #" + std::to_string(*id) + " 正在关闭,按键没送出去。");
        }
        if (!platform::PtyWriteAll(session->master_fd, bytes, 5000)) {
            return Render(*session, history, "按键没写完(终端输入缓冲 5 秒没腾出空,程序可能没在读输入)。", true);
        }
    }
    WaitSettled(*session, wait_ms, start_seq, cancel);
    return Render(*session, history, std::string());
}

Tool::Result DoRead(const nlohmann::json& input, const std::atomic<bool>* cancel) {
    std::string error;
    const auto id = ReadSessionId(input, error);
    if (!id.has_value()) {
        return Tool::Result::Error(error);
    }
    long long wait_ms = 0;
    long long history = 0;
    if (!ReadIntParam(input, "wait_ms", wait_ms, error) || !ReadIntParam(input, "history_lines", history, error)) {
        return Tool::Result::Error(error);
    }
    wait_ms = std::clamp(wait_ms, 0LL, kMaxWaitMs);
    history = std::clamp(history, 0LL, kMaxHistoryLines);
    auto session = Manager::Instance().Find(*id);
    if (session == nullptr) {
        return NoSuchSession(*id);
    }
    std::uint64_t start_seq = 0;
    {
        std::lock_guard<std::mutex> lock(session->mu);
        start_seq = session->output_seq;
    }
    WaitSettled(*session, wait_ms, start_seq, cancel);
    return Render(*session, history, std::string());
}

Tool::Result DoClose(const nlohmann::json& input) {
    std::string error;
    const auto id = ReadSessionId(input, error);
    if (!id.has_value()) {
        return Tool::Result::Error(error);
    }
    auto session = Manager::Instance().Remove(*id);
    if (session == nullptr) {
        return NoSuchSession(*id);
    }
    Terminate(*session);
    return Render(*session, 0, "已关闭终端会话 #" + std::to_string(*id) + "(最后一屏如下)。");
}

Tool::Result DoList() {
    const auto all = Manager::Instance().All();
    if (all.empty()) {
        return Tool::Result::Text("当前没有 terminal 会话。");
    }
    std::ostringstream oss;
    nlohmann::json list = nlohmann::json::array();
    for (const auto& s : all) {
        std::lock_guard<std::mutex> lock(s->mu);
        oss << "#" << s->id << " pid=" << s->pid << " · " << StatusLabel(*s) << " · " << s->rows << "x" << s->cols
            << " · " << s->shell << " · cwd " << s->cwd << "\n";
        list.push_back({{"session_id", s->id}, {"pid", s->pid}, {"running", !s->exited}});
    }
    Tool::Result result = Tool::Result::Text(oss.str());
    result.details["sessions"] = list;
    return result;
}

}  // namespace

Tool::Result TerminalSessionTool::Run(const nlohmann::json& input, const std::atomic<bool>* cancel) {
    if (!input.is_object()) {
        return Result::Error("入参得是对象");
    }
    const auto it = input.find("action");
    if (it == input.end() || !it->is_string()) {
        return Result::Error("缺 action(open/send/read/close/list)");
    }
    const std::string action = it->get<std::string>();
    if (action == "open") {
        return DoOpen(input, cancel);
    }
    if (action == "send") {
        return DoSend(input, cancel);
    }
    if (action == "read") {
        return DoRead(input, cancel);
    }
    if (action == "close") {
        return DoClose(input);
    }
    if (action == "list") {
        return DoList();
    }
    return Result::Error("不认识的 action: " + action + "(可用 open/send/read/close/list)");
}

void CloseAllTerminalSessions() {
    Manager::Instance().CloseAll();
}

#endif  // _WIN32

}  // namespace lubancode::tools
