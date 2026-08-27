// process 运行时实现。基建全在 platform/process(ChildProcess:三管 +
// Job Object/进程组 + 温和终止/杀树);这里只做协议层的组装——请求帧
// 序列化、stdin 一次性写完即关、stdout/stderr 分账、上限落锤、终态分型。
#include "runtime/plugin_process.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/text_encoding.hpp"  // SanitizeExternalText:stderr 尾巴的编码关口

namespace lubancode::runtime {

namespace {

// env 最小集(单子「进程执行与资源边界」+ 第 8 步的硬保证):子进程只见
// 这张表里的变量——PATH 与必要系统变量保底,manifest allowlist 点名的
// 再加,宿主整份环境(连 API key)一概不递。EnvMode::Replace 落的锤:
// 不再是"继承全部 + 额外注入",而是清空后只给这几样。
//   - PATH:解释器/脚本要按名找(python3 这类 command 全靠它);
//   - Windows:SystemRoot/SystemDrive/COMSPEC/WINDIR(缺了 CRT/网络栈
//     起不来);POSIX:LANG/LC_ALL 这类 locale 不递(脚本要中文输出自己
//     reconfigure,协议线本就钉 UTF-8)。
// manifest 没点名的用户变量不递;密钥一概不递。
std::vector<std::pair<std::string, std::string>> BuildProcessEnv(const PluginManifest& manifest) {
    std::vector<std::pair<std::string, std::string>> env;
    auto add_if_present = [&env](const char* name) {
        const auto value = platform::GetEnvVar(name);
        if (value.has_value()) {
            env.emplace_back(name, *value);
        }
    };
    add_if_present("PATH");
#ifdef _WIN32
    add_if_present("SystemRoot");
    add_if_present("SystemDrive");
    add_if_present("COMSPEC");
    add_if_present("WINDIR");
    add_if_present("TEMP");
    add_if_present("TMP");
#else
    add_if_present("TMPDIR");
    add_if_present("HOME");  // POSIX 工具链的常规预期;不含密钥
#endif
    for (const std::string& name : manifest.env_allowlist) {
        const auto value = platform::GetEnvVar(name.c_str());
        if (value.has_value()) {
            // 同名覆盖保底集(PATH 之类被 allowlist 点名时按宿主值来)。
            for (auto& [key, value_ref] : env) {
                if (key == name) {
                    value_ref = *value;
                    break;
                }
            }
            env.emplace_back(name, *value);
        }
    }
    // 同名条目去重(后写的赢;ChildProcess 的表是 vector,重复键行为未定)。
    std::vector<std::pair<std::string, std::string>> dedup;
    dedup.reserve(env.size());
    for (auto& [key, value] : env) {
        bool seen = false;
        for (auto& [dk, dv] : dedup) {
            if (dk == key) {
                dv = value;
                seen = true;
                break;
            }
        }
        if (!seen) {
            dedup.emplace_back(key, value);
        }
    }
    return dedup;
}

}  // namespace

ProcessCallOutcome RunProcessToolCall(const PluginManifest& manifest,
                                      const plugin_protocol::ProcessRequest& request, const std::string& cwd_utf8,
                                      const std::atomic<bool>* cancel, const ProcessCallLimits& limits,
                                      std::string* stderr_tail_out) {
    ProcessCallOutcome outcome;

    // ---- 请求帧 ----
    const nlohmann::json request_json = plugin_protocol::SerializeRequest(request);
    const std::string request_text = request_json.dump();
    if (request_text.size() > limits.stdin_cap_bytes) {
        outcome.code = PluginErrorCode::OutputTooLarge;
        outcome.detail = "请求帧超过 stdin 字节帽(" + std::to_string(request_text.size()) + " > " +
                         std::to_string(limits.stdin_cap_bytes) + "),不发送";
        return outcome;
    }

    // ---- stdout/stderr 分账(读线程回调里攒,帽在回调里落) ----
    struct CaptureState {
        std::mutex mutex;
        std::string stdout_bytes;
        std::string stderr_bytes;
        bool stdout_over_cap = false;
        bool stderr_over_cap = false;
        bool stdout_done = false;  // on_stdout 返回 false 之后 ChildProcess 自己停读
    };
    CaptureState capture;
    const auto on_stdout = [&capture, cap = limits.stdout_cap_bytes](std::string_view chunk) -> bool {
        std::lock_guard<std::mutex> lock(capture.mutex);
        if (capture.stdout_bytes.size() < cap) {
            const std::size_t room = cap - capture.stdout_bytes.size();
            capture.stdout_bytes.append(chunk.data(), std::min(chunk.size(), room));
        }
        if (capture.stdout_bytes.size() >= cap) {
            capture.stdout_over_cap = true;
            capture.stdout_done = true;
            return false;  // 停读:回 false 让 ChildProcess 弃流,主循环杀树
        }
        return true;
    };
    const auto on_stderr = [&capture, cap = limits.stderr_cap_bytes](std::string_view chunk) {
        std::lock_guard<std::mutex> lock(capture.mutex);
        if (capture.stderr_bytes.size() < cap) {
            const std::size_t room = cap - capture.stderr_bytes.size();
            capture.stderr_bytes.append(chunk.data(), std::min(chunk.size(), room));
        }
        // stderr 是日志,超帽只截断保留前段,继续读空管道(不停读——停了
        // 子进程会卡在写上死不掉),也不杀进程。
    };

    // ---- 起进程 ----
    // cwd:缺省项目根(调用方传)。目录不存在(项目根被删/单测环境差异)
    // 时退回继承宿主 cwd,不起失败——cwd 是便利项不是安全边界,进程本就
    // 拿着当前用户的全部权限。
    std::string effective_cwd = cwd_utf8;
    if (!effective_cwd.empty()) {
        std::error_code cwd_ec;
        const std::filesystem::path cwd_path = std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t*>(effective_cwd.data()), effective_cwd.size()));
        if (!std::filesystem::is_directory(cwd_path, cwd_ec)) {
            effective_cwd.clear();
        }
    }
    platform::ChildProcess child;
    const std::vector<std::pair<std::string, std::string>> env = BuildProcessEnv(manifest);
    const auto spawn = child.Start(manifest.argv[0],
                                   std::vector<std::string>(manifest.argv.begin() + 1, manifest.argv.end()), env,
                                   on_stdout, on_stderr, effective_cwd, platform::EnvMode::Replace);
    if (!spawn.success) {
        outcome.code = PluginErrorCode::SpawnFailed;
        outcome.detail = "起插件进程失败(" + manifest.argv[0] + "): " + spawn.error;
        return outcome;
    }

    // ---- stdin:一次性写完即关 ----
    // 写完必须关写端:脚本要靠 EOF 收束 json.load(sys.stdin) 这类读法,
    // 不关写端子进程永远等在 read 上(实测 30s 超时全是这个坑)。
    // Write 本身是阻塞写;子进程不读 stdin 而数据超过管道缓冲时写会卡住
    // ——所以整趟放在独立线程,主循环的超时/取消照样落锤(杀树后读端
    // 关,write 以 EPIPE 收场,写线程退)。
    std::atomic<bool> stdin_written{false};
    std::thread stdin_writer([&child, &request_text, &stdin_written] {
        child.Write(request_text);  // 失败(EPIPE 等)静默:主循环按协议错收场
        child.CloseStdin();         // 写完即关:EOF 是协议的一部分
        stdin_written.store(true);
    });

    // 超时:manifest 是插件作者给的墙,运行时不得放宽——取较小者。
    const int effective_timeout =
        limits.timeout_ms > 0 && manifest.timeout_ms > 0 ? std::min(limits.timeout_ms, manifest.timeout_ms)
                                                         : std::max(limits.timeout_ms, manifest.timeout_ms);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(effective_timeout > 0 ? effective_timeout : 0);
    const bool has_deadline = effective_timeout > 0;
    const int grace_ms = 2000;  // 温和终止后的宽限,过线杀整棵树

    // ---- 主循环:等退出 / 超时 / 取消 / stdout 超帽 ----
    bool timed_out = false;
    bool cancelled = false;
    bool stdout_overflow = false;
    while (true) {
        if (!child.IsAlive()) {
            break;
        }
        {
            std::lock_guard<std::mutex> lock(capture.mutex);
            if (capture.stdout_over_cap) {
                stdout_overflow = true;
                break;
            }
        }
        if (cancel != nullptr && cancel->load()) {
            cancelled = true;
            break;
        }
        if (has_deadline && std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // ---- 收尾:ChildProcess::Shutdown 先关 stdin(EOF),等 wait_ms;
    // 还没退就 KillProcessGroup/关 Job——整棵树杀干净,绝不留孤儿。幂等,
    // 进程已退出时只是收线程关句柄。----
    child.Shutdown(grace_ms);

    // stdin 写线程收尸:进程死透后 Write 的阻塞以 EPIPE/句柄失效收场;
    // 限时兜底等 2s,等不到(极端:还有后代握着 stdin 读端)再等 2s 后
    // detach 放走。detach 的风险窗口(写线程还引用栈上的 child)是有界的:
    // Shutdown 后 stdin 句柄已关,Write 立刻失败返回,线程随即收尾;
    // Windows 对已关句柄的 WriteFile 返回 FALSE,POSIX 对已关 fd 返回
    // EBADF,都不崩。正常路径(进程死透、管道断)写线程早就自己退了。
    {
        const auto stdin_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (!stdin_written.load() && std::chrono::steady_clock::now() < stdin_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (stdin_written.load()) {
            stdin_writer.join();
        } else if (stdin_writer.joinable()) {
            stdin_writer.detach();
        }
    }

    const int exit_code = child.exit_code();
    outcome.exit_code = exit_code;
    {
        std::lock_guard<std::mutex> lock(capture.mutex);
        outcome.stderr_tail = capture.stderr_bytes;
    }
    if (stderr_tail_out != nullptr) {
        *stderr_tail_out = outcome.stderr_tail;
    }

    // ---- 终态分型(唯一宿主错误码) ----
    if (cancelled) {
        outcome.code = PluginErrorCode::Cancelled;
        outcome.detail = "用户取消(ESC):进程树已终止";
        return outcome;
    }
    if (timed_out) {
        outcome.code = PluginErrorCode::TimedOut;
        outcome.detail = "插件进程超时(" + std::to_string(effective_timeout) + "ms),进程树已终止";
        return outcome;
    }
    if (stdout_overflow) {
        outcome.code = PluginErrorCode::OutputTooLarge;
        outcome.detail = "stdout 超过字节帽(" + std::to_string(limits.stdout_cap_bytes) + "B),已停读并终止进程";
        return outcome;
    }
    // 崩溃/被信号杀:Windows 的 exit_code 无信号概念,POSIX 记负数信号。
    // 子进程被我们杀树时也会走信号路径——那已被上面三个分型收走,到这里
    // 的信号死都算插件自己崩的。
    if (exit_code < 0) {
        outcome.code = PluginErrorCode::ToolSignaled;
        outcome.detail = "插件进程被信号杀死(信号 " + std::to_string(-exit_code) + ")";
        return outcome;
    }
    if (exit_code != 0) {
        outcome.code = PluginErrorCode::ToolExitNonZero;
        outcome.detail = "插件进程非零退出(" + std::to_string(exit_code) + ")";
        if (!outcome.stderr_tail.empty()) {
            // stderr 是外来字节:中文 Windows 上插件走本地代码页吐字,原样
            // 塞进 detail 就是非法 UTF-8 进 tool_result(316 砖死那类病)。
            // 按仓库编码政策先过清洗(ACP 试转一把,转不动逐段 U+FFFD)。
            outcome.detail += ";stderr 尾巴: " +
                              platform::SanitizeExternalText(outcome.stderr_tail.substr(0, 512));
        }
        return outcome;
    }

    // ---- 协议解析(三关:UTF-8 / 恰好一份 JSON / call_id) ----
    std::string stdout_bytes;
    {
        std::lock_guard<std::mutex> lock(capture.mutex);
        stdout_bytes = capture.stdout_bytes;
    }
    const auto parsed = plugin_protocol::ParseResponse(stdout_bytes, request.call_id);
    if (parsed.status != PluginErrorCode::Ok) {
        outcome.code = parsed.status;
        outcome.detail = parsed.detail;
        return outcome;
    }
    if (!parsed.response.ok) {
        outcome.code = PluginErrorCode::PluginReportedError;
        outcome.plugin_error_code = parsed.response.error_code;
        outcome.detail = parsed.response.error_message;
        outcome.text = parsed.response.error_message;  // 插件自报的错也是给模型的话
        return outcome;
    }
    outcome.code = PluginErrorCode::Ok;
    outcome.text = parsed.response.text;
    outcome.images = std::move(parsed.response.images);
    outcome.structured = parsed.response.structured;
    return outcome;
}

}  // namespace lubancode::runtime
