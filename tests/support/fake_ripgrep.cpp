// 假 ripgrep(ripgrep 迁移单 P0-4 的流式 runner 测试夹具)。
//
// 一枚跨平台小可执行:argv 里找 `--` 之后的 pattern,按 `@场景名` 前缀选
// 剧情,把伪造的 rg 输出喂给 stdout/stderr。BundledRipgrepRunner 经
// exe_override 起它,就能在单测里覆盖真 rg 不肯配合的路径:
//   @jsonl-basic      begin/match*2/end/summary,退出 0
//   @no-match         只有 summary,退出 1(无命中走成功)
//   @invalid-regex    stderr 写 regex parse error,退出 2
//   @exit7            吐一条 match 后退出 7(进程半途死,不冒充无命中)
//   @bad-json         吐一行不是 JSON 的东西(协议错收树)
//   @big-frame        吐 2 MiB 无换行字节(1 MiB 未完成帧超帽)
//   @many-hits:N      吐 N 条 match(逐条冲刷),再睡 300ms 后写 marker 文件
//                     退出 0——满额主动收树的测试看 marker 在不在:在=rg 跑
//                     完了全程(没提前停),不在=第 N 条就被杀
//   @wide-lines:N     吐 N 条各 2 万字符正文的 match(16 KiB 单行截断与
//                     512 KiB 总量墙的原料)
//   @never-exit       吐一条 match 后睡 30 秒(cancel/timeout 的靶子)
//   @stderr-flood     stderr 灌 100 KiB + stdout 一条 summary,退出 0
//   @tail-no-newline  吐一条不带尾换行的 match JSON 后退出 0(尾帧合同)
//   @spawn-child      自己再生一个孩子(孩子把 PID 写进 marker 后睡 30 秒),
//                     自己也睡 30 秒——取消收树"不留孤儿"的夹具:Windows 侧
//                     孩子 Job Object 连坐,POSIX 侧进程组连坐
//   @child-sleep      (内部)把 PID 写进 marker 睡 30 秒,@spawn-child 的孩子
//
// marker 文件路径从环境变量 LUBANCODE_FAKE_RG_MARKER 读(ChildProcess 默认
// 继承宿主环境)。输出全部行级冲刷——流式 runner 的满额判断依赖"读到的
// 每一条都已到管道"。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace {

std::string MarkerPath() {
    const char* marker = std::getenv("LUBANCODE_FAKE_RG_MARKER");
    return marker != nullptr ? std::string(marker) : std::string();
}

void WriteMarker(const std::string& content) {
    const std::string path = MarkerPath();
    if (path.empty()) {
        return;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return;
    }
    std::fputs(content.c_str(), f);
    std::fclose(f);
}

void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

long OwnPid() {
#ifdef _WIN32
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(getpid());
#endif
}

// 找 argv 里的场景名:以 '@' 开头的那一枚。grep 模式 pattern 落在 `--`
// 之后,但 glob 模式 pattern 是 `-g` 的值——两边都是"唯一以 @ 开头的参数",
// 按这个特征扫最稳(其余 argv 是 flag 或 ".")。
std::string PatternFromArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '@') {
            return argv[i];
        }
    }
    return std::string();
}

void Emit(const std::string& line) {
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
}

// 手拼 JSON 的转义:路径/正文里的反斜杠(Windows 路径!)与双引号必须
// 转义,真实换行换成 \n 转义——不然生成的是坏 JSON,流式 runner 会按
// 协议错收树(这本来就是 @bad-json 场景要考的东西,别的场景别误伤)。
std::string JsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
        }
    }
    return out;
}

std::string MatchEvent(const std::string& path, long long line_number, const std::string& text) {
    return "{\"type\":\"match\",\"data\":{\"path\":{\"text\":\"" + JsonEscape(path) +
           "\"},\"lines\":{\"text\":\"" + JsonEscape(text) +
           "\"},\"line_number\":" + std::to_string(line_number) + ",\"submatches\":[]}}\n";
}

const char* kSummary =
    "{\"data\":{\"elapsed_total\":{\"human\":\"0.0001s\",\"nanos\":100000,\"secs\":0},"
    "\"stats\":{\"bytes_printed\":0,\"bytes_searched\":0,\"matched_lines\":1,\"matches\":1,"
    "\"searches\":1,\"searches_with_match\":1}},\"type\":\"summary\"}\n";

// 再生一个孩子并把它的 PID 写进 marker;孩子睡 30 秒(测试用
// IsProcessAlive 验它已被收树)。返回前自己也睡 30 秒。
int SpawnChildAndSleep() {
#ifdef _WIN32
    // 自举:CreateProcessW 起自己跑 @child-sleep。孩子在同一个 Job Object
    // 里(子进程默认继承 job)——TerminateJobObject 一锅端,这就是要证明的。
    char exe[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH) == 0) {
        return 1;
    }
    char cmdline[MAX_PATH + 64];
    std::snprintf(cmdline, sizeof(cmdline), "\"%s\" -- @child-sleep", exe);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmdline, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    // fork 不 exec:孩子留在同一进程组,killpg SIGTERM→SIGKILL 连坐。
    const pid_t pid = fork();
    if (pid == 0) {
        WriteMarker(std::to_string(static_cast<long>(getpid())) + "\n");
        SleepMs(30'000);
        _exit(0);
    }
    if (pid < 0) {
        return 1;
    }
#endif
    SleepMs(30'000);
    return 0;
}

int ChildSleep() {
    WriteMarker(std::to_string(OwnPid()) + "\n");
    SleepMs(30'000);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string pattern = PatternFromArgs(argc, argv);

    if (pattern == "@child-sleep") {
        return ChildSleep();
    }
    if (pattern == "@spawn-child") {
        return SpawnChildAndSleep();
    }
    if (pattern == "@jsonl-basic") {
        Emit("{\"type\":\"begin\",\"data\":{\"path\":{\"text\":\".\\\\a.txt\"}}}\n");
        // 正文给真实换行,JSON 转义交给 JsonEscape——宿主解析器按 rg 合同
        // 剥掉行尾换行后应得 "needle one"。
        Emit(MatchEvent(".\\a.txt", 2, "needle one\n"));
        Emit(MatchEvent(".\\a.txt", 4, "needle two\n"));
        Emit("{\"type\":\"end\",\"data\":{\"path\":{\"text\":\".\\\\a.txt\"}}}\n");
        Emit(kSummary);
        return 0;
    }
    if (pattern == "@no-match") {
        Emit(kSummary);
        return 1;
    }
    if (pattern == "@invalid-regex") {
        std::fputs("rg: regex parse error:\n    (?:(unclosed)\n    ^\nerror: unclosed group\n", stderr);
        std::fflush(stderr);
        return 2;
    }
    if (pattern == "@exit7") {
        Emit(MatchEvent("dying.txt", 1, "partial output then crash\n"));
        return 7;
    }
    if (pattern == "@bad-json") {
        Emit("this is not json at all\n");
        return 0;
    }
    if (pattern == "@big-frame") {
        // 2 MiB 无换行:第四道墙(1 MiB 未完成帧)应当在写到一半时触发,
        // 宿主收树,这段循环根本跑不完——跑到完也不影响测试判停。
        const std::string chunk(64 * 1024, 'x');
        for (int i = 0; i < 32; ++i) {
            std::fputs(chunk.c_str(), stdout);
            std::fflush(stdout);
        }
        return 0;
    }
    if (pattern.rfind("@many-hits:", 0) == 0) {
        const int n = std::atoi(pattern.c_str() + std::strlen("@many-hits:"));
        for (int i = 0; i < n; ++i) {
            Emit(MatchEvent("f" + std::to_string(i) + ".txt", 1, "hit line\n"));
        }
        // 满额停树的铁证:睡 300ms 再写 marker。宿主在第 100 条(或注入的
        // 小帽)就收树的话,这行永远执行不到。
        SleepMs(300);
        WriteMarker("completed\n");
        return 0;
    }
    if (pattern.rfind("@wide-lines:", 0) == 0) {
        const int n = std::atoi(pattern.c_str() + std::strlen("@wide-lines:"));
        const std::string wide(20'000, 'w');
        for (int i = 0; i < n; ++i) {
            Emit(MatchEvent("w" + std::to_string(i) + ".txt", 1, wide + "\n"));  // 真实换行由 JsonEscape 转义
        }
        SleepMs(300);
        WriteMarker("completed\n");
        return 0;
    }
    if (pattern == "@never-exit") {
        // 先把 PID 写进 marker(测试用它验"取消后进程表无残留"),再吐一条
        // match,然后长睡——cancel/timeout 的靶子。
        WriteMarker(std::to_string(OwnPid()) + "\n");
        Emit(MatchEvent("slow.txt", 1, "first hit\n"));
        SleepMs(30'000);
        return 0;
    }
    if (pattern.rfind("@glob-many:", 0) == 0) {
        // glob 模式:NUL 分帧吐 N 条路径(最后一条不带尾 NUL,顺带考尾帧),
        // 睡 300ms 后写 marker——glob 的满额停树同 @many-hits 的口径。
        const int n = std::atoi(pattern.c_str() + std::strlen("@glob-many:"));
        for (int i = 0; i < n; ++i) {
            std::fputs(("dir/f" + std::to_string(i) + ".txt").c_str(), stdout);
            if (i + 1 < n) {
                std::fputc('\0', stdout);
            }
        }
        std::fflush(stdout);
        SleepMs(300);
        WriteMarker("completed\n");
        return 0;
    }
    if (pattern == "@glob-files") {
        // 三条路径,含反斜杠与 ./ 前缀(考 NormalizeRipgrepPath),尾帧带 NUL。
        // fputs 遇内嵌 NUL 会截断,必须 fwrite 带长度。
        const std::string paths = std::string(".\\a.txt") + '\0' + "sub/b.txt" + '\0' +
                                  ".\\deep\\c.md" + '\0';
        std::fwrite(paths.data(), 1, paths.size(), stdout);
        std::fflush(stdout);
        return 0;
    }
    if (pattern == "@stderr-flood") {
        const std::string noise(2'000, 'e');
        for (int i = 0; i < 52; ++i) {  // ~104 KiB,压过 64 KiB stderr 帽
            std::fputs((noise + "\n").c_str(), stderr);
        }
        std::fflush(stderr);
        Emit(kSummary);
        return 0;
    }
    if (pattern == "@tail-no-newline") {
        const std::string event = MatchEvent("tail.txt", 3, "tail without newline\n");
        std::fputs(event.substr(0, event.size() - 1).c_str(), stdout);  // 去掉最后的 '\n'
        std::fflush(stdout);
        return 0;
    }

    // 未认得的场景:安静退出 0(测试写错场景名时这里能看出"没输出")。
    return 0;
}
