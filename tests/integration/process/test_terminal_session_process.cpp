// terminal 工具真 PTY 用例(仅 POSIX):起 bash、送按键、看屏、收会话。
// 时序一律"轮询屏幕直到出现或超时",不死睡;失败时 CAPTURE 最后一屏。
// python3/vim 不在机器上就 MESSAGE 说明后跳过该段。
#include <doctest/doctest.h>

#ifndef _WIN32

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "tools/terminal_session_tool.hpp"

using lubancode::tools::CloseAllTerminalSessions;
using lubancode::tools::TerminalSessionTool;
using lubancode::tools::Tool;

namespace {

constexpr auto kPollTimeout = std::chrono::seconds(15);

// 从工具结果里抠出屏幕正文(两条分隔线之间)。
std::string ScreenOf(const std::string& content) {
    const std::string begin = "---- 屏幕 ----\n";
    const std::string end = "\n---- 屏幕完 ----";
    const std::size_t b = content.find(begin);
    if (b == std::string::npos) {
        return std::string();
    }
    const std::size_t start = b + begin.size();
    const std::size_t e = content.find(end, start);
    return content.substr(start, e == std::string::npos ? std::string::npos : e - start);
}

std::vector<std::string> LinesOf(const std::string& screen) {
    std::vector<std::string> out;
    std::istringstream in(screen);
    std::string line;
    while (std::getline(in, line)) {
        out.push_back(line);
    }
    return out;
}

int CountExactLines(const std::string& screen, const std::string& wanted) {
    int n = 0;
    for (const auto& line : LinesOf(screen)) {
        if (line == wanted) {
            ++n;
        }
    }
    return n;
}

bool HasExactLine(const std::string& screen, const std::string& wanted) {
    return CountExactLines(screen, wanted) > 0;
}

bool CommandExists(const char* name) {
    const std::string probe = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(probe.c_str()) == 0;
}

// 进程是否已不在:ESRCH 即无;Linux 上僵尸(等 init 收尸)也算不在。
bool ProcessGone(int pid) {
    if (::kill(pid, 0) != 0 && errno == ESRCH) {
        return true;
    }
#ifdef __linux__
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string content;
    std::getline(stat, content);
    const std::size_t paren = content.rfind(')');
    if (paren != std::string::npos && paren + 2 < content.size() && content[paren + 2] == 'Z') {
        return true;
    }
#endif
    return false;
}

bool WaitProcessGone(int pid) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ProcessGone(pid)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return ProcessGone(pid);
}

struct Term {
    TerminalSessionTool tool;
    int id = -1;
    int pid = -1;
    std::string last;  // 最近一次结果原文,失败时亮出来

    Tool::Result Call(const nlohmann::json& input) {
        Tool::Result result = tool.execute(input);
        last = result.content;
        return result;
    }

    void Open(const std::string& cwd = std::string()) {
        nlohmann::json input{{"action", "open"}, {"wait_ms", 1000}};
        if (!cwd.empty()) {
            input["cwd"] = cwd;
        }
        Tool::Result result = Call(input);
        CAPTURE(result.content);
        REQUIRE_FALSE(result.is_error);
        REQUIRE(result.details.contains("session_id"));
        REQUIRE(result.details.contains("pid"));
        id = result.details.at("session_id").get<int>();
        pid = result.details.at("pid").get<int>();
        CHECK(pid > 0);
    }

    Tool::Result Send(const std::string& keys, int wait_ms = 300) {
        return Call(nlohmann::json{{"action", "send"}, {"session_id", id}, {"keys", keys}, {"wait_ms", wait_ms}});
    }

    // 轮询屏幕直到谓词成立或超时。
    bool WaitFor(const std::function<bool(const Tool::Result&, const std::string&)>& pred,
                 std::chrono::milliseconds timeout = kPollTimeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            Tool::Result result = Call(nlohmann::json{{"action", "read"}, {"session_id", id}, {"wait_ms", 200}});
            if (pred(result, ScreenOf(result.content))) {
                return true;
            }
        }
        return false;
    }

    bool WaitLine(const std::string& wanted, std::chrono::milliseconds timeout = kPollTimeout) {
        return WaitFor([&wanted](const Tool::Result&, const std::string& screen) { return HasExactLine(screen, wanted); },
                       timeout);
    }

    Tool::Result Close() { return Call(nlohmann::json{{"action", "close"}, {"session_id", id}}); }
};

std::filesystem::path MakeTempDir(const std::string& tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode_terminal_" + tag + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST_CASE("terminal: open bash 后 echo 结果上屏,close 后进程不在") {
    Term term;
    term.Open();
    term.Send("echo $((40+2))<Enter>");
    const bool seen = term.WaitLine("42");
    CAPTURE(term.last);
    CHECK(seen);

    const int pid = term.pid;
    const Tool::Result closed = term.Close();
    CAPTURE(closed.content);
    CHECK_FALSE(closed.is_error);
    CHECK(WaitProcessGone(pid));

    // 关过的会话再 read:明确报没有
    const Tool::Result again = term.Call(nlohmann::json{{"action", "read"}, {"session_id", term.id}});
    CHECK(again.is_error);
}

TEST_CASE("terminal: cat 回显与 Ctrl-D 收尾,Ctrl-C 打断前台作业") {
    Term term;
    term.Open();
    term.Send("cat<Enter>");
    term.Send("abc<Enter>");
    // 线路规程回显一行 + cat 原样吐一行
    const bool echoed =
        term.WaitFor([](const Tool::Result&, const std::string& screen) { return CountExactLines(screen, "abc") >= 2; });
    CAPTURE(term.last);
    CHECK(echoed);

    term.Send("<C-d>");
    term.Send("echo done-$((1+1))<Enter>");
    const bool back = term.WaitLine("done-2");
    CAPTURE(term.last);
    CHECK(back);

    // <C-c> 若抢在 sleep 成为前台作业之前到达,只是 bash 提示符上的一次空
    // 中断,sleep 照睡;所以"打断 + 探针"成组重试,探针出现即证打断生效
    //(SIGINT 还会冲掉 tty 里预输入的探针文本,下一轮重送)。
    term.Send("sleep 30<Enter>");
    bool interrupted = false;
    for (int attempt = 0; attempt < 5 && !interrupted; ++attempt) {
        term.Send("<C-c>");
        term.Send("echo after-$((2+3))<Enter>");
        interrupted = term.WaitLine("after-5", std::chrono::milliseconds(2500));
    }
    CAPTURE(term.last);
    CHECK(interrupted);

    term.Close();
}

TEST_CASE("terminal: python3 REPL 求值") {
    if (!CommandExists("python3")) {
        MESSAGE("SKIP: 机器上没有 python3");
        return;
    }
    Term term;
    term.Open();
    term.Send("python3 -q<Enter>");
    const bool prompt = term.WaitFor([](const Tool::Result&, const std::string& screen) {
        for (const auto& line : LinesOf(screen)) {
            if (line.rfind(">>>", 0) == 0) {
                return true;
            }
        }
        return false;
    });
    CAPTURE(term.last);
    REQUIRE(prompt);

    term.Send("111*3<Enter>");
    const bool answered = term.WaitLine("333");
    CAPTURE(term.last);
    CHECK(answered);

    term.Send("exit()<Enter>");
    term.Close();
}

TEST_CASE("terminal: vim 进备用屏写文件,:wq 后回主屏且文件落盘") {
    if (!CommandExists("vim")) {
        MESSAGE("SKIP: 机器上没有 vim");
        return;
    }
    const auto dir = MakeTempDir("vim");
    Term term;
    term.Open(dir.string());
    term.Send("vim -u NONE -N a.txt<Enter>");
    const bool in_vim = term.WaitFor([](const Tool::Result& result, const std::string& screen) {
        return result.details.contains("alternate_screen") && result.details.at("alternate_screen").get<bool>() &&
               HasExactLine(screen, "~");
    });
    CAPTURE(term.last);
    REQUIRE(in_vim);

    term.Send("ihello terminal<Esc>:wq<Enter>");
    const bool left = term.WaitFor([](const Tool::Result& result, const std::string&) {
        return result.details.contains("alternate_screen") && !result.details.at("alternate_screen").get<bool>();
    });
    CAPTURE(term.last);
    CHECK(left);

    // vim 退出与落盘有先后,轮询文件
    std::string content;
    const auto deadline = std::chrono::steady_clock::now() + kPollTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream in(dir / "a.txt");
        std::getline(in, content);
        if (content == "hello terminal") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(content == "hello terminal");

    term.Close();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("terminal: shell 自己退出后快照标出退出码,send 报错") {
    Term term;
    term.Open();
    term.Send("exit 3<Enter>");
    const bool exited = term.WaitFor([](const Tool::Result& result, const std::string&) {
        return result.details.contains("running") && !result.details.at("running").get<bool>();
    });
    CAPTURE(term.last);
    REQUIRE(exited);
    const Tool::Result status = term.Call(nlohmann::json{{"action", "read"}, {"session_id", term.id}});
    REQUIRE(status.details.contains("exit_code"));
    CHECK(status.details.at("exit_code").get<int>() == 3);
    CHECK(term.Send("echo nope<Enter>").is_error);
    term.Close();
}

TEST_CASE("terminal: 不认识的按键记号不发送并报错") {
    Term term;
    term.Open();
    const Tool::Result bad = term.Send("echo x<Entr>");
    CHECK(bad.is_error);
    CHECK(bad.content.find("<Entr>") != std::string::npos);
    term.Close();
}

TEST_CASE("terminal: 会话名额上限,收尾 CloseAll 全部收净") {
    CloseAllTerminalSessions();
    TerminalSessionTool tool;
    std::vector<int> pids;
    for (std::size_t i = 0; i < lubancode::tools::kMaxTerminalSessions; ++i) {
        const Tool::Result r = tool.execute(nlohmann::json{{"action", "open"}, {"wait_ms", 0}});
        CAPTURE(r.content);
        REQUIRE_FALSE(r.is_error);
        pids.push_back(r.details.at("pid").get<int>());
    }
    const Tool::Result over = tool.execute(nlohmann::json{{"action", "open"}, {"wait_ms", 0}});
    CAPTURE(over.content);
    CHECK(over.is_error);

    const Tool::Result listed = tool.execute(nlohmann::json{{"action", "list"}});
    REQUIRE(listed.details.contains("sessions"));
    CHECK(listed.details.at("sessions").size() == lubancode::tools::kMaxTerminalSessions);

    CloseAllTerminalSessions();
    for (const int pid : pids) {
        CAPTURE(pid);
        CHECK(WaitProcessGone(pid));
    }
    const Tool::Result empty = tool.execute(nlohmann::json{{"action", "list"}});
    CHECK_FALSE(empty.details.contains("sessions"));
}

#else  // _WIN32

#include <nlohmann/json.hpp>

#include "tools/terminal_session_tool.hpp"

TEST_CASE("terminal: Windows 上不做 ConPTY,工具回仅 POSIX") {
    lubancode::tools::TerminalSessionTool tool;
    CHECK(tool.execute(nlohmann::json{{"action", "open"}}).is_error);
}

#endif  // _WIN32
