// PtcRunner 的实现:沙箱子进程、framed RPC 服务循环、五道上限、审计账。
//
// 线程模型:
//   - ChildProcess 的 stdout 读线程只负责分帧入队(回调里不做重活);
//   - 执行(宿主工具链)全部在调用 Run() 的线程上——与 JSON 工具调用同
//     一条线程语义,UI/确认/钩子回调不必改线程安全假设;
//   - 取消(Esc)与墙钟看门狗由本线程在循环边界检查,不另起看门狗线程:
//     cv 按 250ms 一跳,响应足够,也免去"看门狗线程杀进程时主线程还在
//     写管道"的竞态——收场全在本线程串行做完。

#include "ptc/runner.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

#include "platform/process.hpp"
#include "ptc/bootstrap_py.hpp"
#include "ptc/protocol.hpp"

namespace lubancode::ptc {

namespace {

using Clock = std::chrono::steady_clock;

// 运行现场的可变状态:读线程生产,主线程消费。
struct ServeState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> pending_frames;  // 完整帧负载
    std::string stderr_text;                  // stderr 累积(截尾在收尾时做)
    std::size_t bytes_in = 0;                 // python -> 宿主帧负载字节
    std::size_t bytes_out = 0;                // 宿主 -> python 帧负载字节
    bool frames_failed = false;               // 分帧器报错(协议墙)
    std::string frames_error;
    Clock::time_point last_frame_at;          // 最近一次收到帧的时刻(EOF 判定)
    std::atomic<int> calls_received{0};       // 并发窗口径:收到的 call 帧总数
    std::atomic<int> responses_sent{0};       // 并发窗口径:已答复的 call 总数
};

// 按需保留临时目录(调脚本排障用):LUBANCODE_PTC_KEEP_DIR 非空即保留。
bool KeepTempDir() {
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, "LUBANCODE_PTC_KEEP_DIR") != 0 || buffer == nullptr) {
        return false;
    }
    const bool keep = buffer[0] != '\0';
    std::free(buffer);
    return keep;
#else
    const char* raw = std::getenv("LUBANCODE_PTC_KEEP_DIR");
    return raw != nullptr && raw[0] != '\0';
#endif
}

// 把文本写进临时目录里的一枚文件。
bool WriteFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

}  // namespace

std::string PtcRunner::NewRunId() {
    static std::atomic<std::uint32_t> counter{0};
    std::random_device rd;
    const std::uint32_t random_part = rd();
    const std::uint32_t seq = counter.fetch_add(1);
    std::ostringstream out;
    out << std::hex << ((random_part ^ (seq * 0x9E3779B9U)) & 0xFFFFFFFFU);
    std::string id = out.str();
    while (id.size() < 8) {
        id.insert(id.begin(), '0');
    }
    if (id.size() > 8) {
        id.resize(8);
    }
    return "ptc-" + id;
}

std::string PtcRunner::StableHashText(const std::string& text) {
    // FNV-1a 64:审计账够用(碰撞去重的辅助键,不做密码学承诺)。
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::string PtcRunner::StableHash(const nlohmann::json& value) {
    // dump 用有序键(nlohmann::json 默认 map 即有序),规范化由此保证。
    return StableHashText(value.dump());
}

std::string PtcRunner::FailureText(PtcFailure failure) {
    switch (failure) {
        case PtcFailure::None: return "成功";
        case PtcFailure::Spawn: return "Python 起不来";
        case PtcFailure::Syntax: return "脚本语法错";
        case PtcFailure::Import: return "脚本 import 错";
        case PtcFailure::Runtime: return "脚本运行时错";
        case PtcFailure::Guard: return "脚本越护栏(沙箱拒绝)";
        case PtcFailure::Rpc: return "RPC 协议错";
        case PtcFailure::LimitWallClock: return "撞墙: 墙钟超时";
        case PtcFailure::LimitCpu: return "撞墙: CPU 时间上限";
        case PtcFailure::LimitMemory: return "撞墙: 内存上限";
        case PtcFailure::LimitOutput: return "撞墙: 输出字节上限";
        case PtcFailure::LimitCalls: return "撞墙: 调用数上限";
        case PtcFailure::LimitConcurrency: return "撞墙: 并发数上限";
        case PtcFailure::Cancelled: return "已取消(Esc)";
        case PtcFailure::Sandbox: return "沙箱终止(资源墙或原生崩溃)";
        case PtcFailure::Protocol: return "帧协议流程错";
    }
    return "未知";
}

std::string PlatformSandboxGrade(bool allow_posix_rlimit_fallback) {
#ifdef _WIN32
    (void)allow_posix_rlimit_fallback;
    return "windows-job+restricted-token";
#else
    if (allow_posix_rlimit_fallback) {
        // rlimit 只限 CPU/内存,不隔离文件系统/网络——不是"可靠沙箱",
        // 默认禁 PTC 的那条硬条件依据的就是它。
        return "posix-rlimit(非可靠沙箱)";
    }
    return "none";
#endif
}

bool PtcRunResult::HasSideEffects() const {
    for (const auto& call : calls) {
        if (call.side_effect_class == "write") {
            return true;
        }
    }
    return false;
}

bool PtcRunResult::HasReadOnlyCalls() const {
    return !calls.empty() && !HasSideEffects();
}

PtcRunResult PtcRunner::Run(const std::string& script, const std::string& stub_module_python, Options options) {
    const auto started_at = Clock::now();
    PtcRunResult result;
    result.ptc_run_id = NewRunId();
    result.script_hash = StableHashText(script);
    result.sandbox_grade = "none";

    if (!options.executor) {
        result.failure = PtcFailure::Rpc;
        result.error = "PtcRunner 没有装配宿主执行链(PtcCallExecutor 为空)";
        return result;
    }

    // ---- 1. 临时目录与四份文件 ----
    std::filesystem::path dir = std::filesystem::path(options.work_dir.empty() ? "" : options.work_dir);
    if (dir.empty()) {
        dir = std::filesystem::temp_directory_path();
    }
    std::error_code ec;
    dir = dir / ("lubancode-ptc-" + result.ptc_run_id);
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        result.failure = PtcFailure::Spawn;
        result.error = "建临时目录失败: " + ec.message();
        return result;
    }
    const auto cleanup = [&dir]() {
        if (!KeepTempDir()) {
            std::error_code remove_ec;
            std::filesystem::remove_all(dir, remove_ec);
        }
    };
    if (!WriteFile(dir / "ptc_runtime.py", kPtcRuntimePython) ||
        !WriteFile(dir / "ptc_main.py", kPtcMainPython) ||
        !WriteFile(dir / "luban_tools.py", stub_module_python) ||
        !WriteFile(dir / "ptc_script.py", script)) {
        result.failure = PtcFailure::Spawn;
        result.error = "写 PTC 临时文件失败: " + dir.string();
        cleanup();
        return result;
    }

    // ---- 2. 起沙箱子进程 ----
    platform::SpawnConstraints constraints;
    if (options.use_os_sandbox) {
        if (options.limits.cpu_ms > 0) {
            constraints.cpu_seconds = std::max(1, options.limits.cpu_ms / 1000);
        }
        if (options.limits.memory_bytes > 0) {
            constraints.memory_bytes = options.limits.memory_bytes;
        }
        constraints.restricted_token = options.restricted_token;
#ifdef _WIN32
        result.sandbox_grade = options.restricted_token ? "windows-job+restricted-token" : "windows-job";
#else
        result.sandbox_grade = "posix-rlimit";
#endif
    }

    ServeState state;
    state.last_frame_at = Clock::now();
    FrameDecoder decoder;

    platform::ChildProcess child;
    const auto on_stdout = [&state, &decoder](std::string_view chunk) -> bool {
        std::vector<std::string> frames;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.frames_failed) {
                return false;  // 已经判协议墙,别再喂
            }
            const auto feed = decoder.Feed(chunk, frames);
            if (!feed.has_value()) {
                state.frames_failed = true;
                state.frames_error = feed.error();
                state.cv.notify_all();
                return false;
            }
            if (!frames.empty()) {
                state.last_frame_at = Clock::now();
                for (const auto& frame : frames) {
                    state.bytes_in += frame.size();
                }
                state.pending_frames.insert(state.pending_frames.end(), frames.begin(), frames.end());
                state.cv.notify_all();
            }
        }
        return true;
    };
    const auto on_stderr = [&state](std::string_view chunk) {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.stderr_text.size() < 256 * 1024) {
            state.stderr_text.append(chunk.data(), chunk.size());
        }
        state.bytes_in += chunk.size();
    };

    const std::string main_path = (dir / "ptc_main.py").string();
    const auto spawn = child.Start(options.python_cmd, {"-I", "-S", "-B", main_path}, {}, on_stdout, on_stderr,
                                   constraints);
    if (!spawn.success) {
        result.failure = PtcFailure::Spawn;
        result.error = "起 Python 失败(" + options.python_cmd + "): " + spawn.error;
        cleanup();
        return result;
    }

    // ---- 3. 服务循环 ----
    bool got_hello = false;
    bool terminal = false;      // done/fail 已到
    bool abort_sent = false;    // Esc 后已发过 abort 帧
    Clock::time_point cancel_deadline{};  // Esc 宽限线(过线杀树)
    int reused_calls = 0;       // 同参只读去重命中数
    std::map<std::pair<std::string, std::string>, std::string> reuse_cache;  // (tool, input_hash) -> content
    const auto deadline = started_at + std::chrono::milliseconds(options.limits.wall_clock_ms);

    const auto send_result = [&](std::uint64_t id, bool ok, const nlohmann::json& value, const std::string& error) {
        const std::string payload = BuildResultPayload(id, ok, value, error);
        state.bytes_out += payload.size();
        state.responses_sent.fetch_add(1);
        child.Write(EncodeFrame(payload));
    };
    const auto finish_call = [&](PtcCallRecord record, bool ok, const std::string& error, int elapsed_ms) {
        record.ok = ok;
        record.error = error;
        record.elapsed_ms = elapsed_ms;
        result.calls.push_back(std::move(record));
    };

    while (!terminal) {
        // 取一批待处理帧。
        std::vector<std::string> batch;
        bool frames_failed = false;
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            batch.swap(state.pending_frames);
            frames_failed = state.frames_failed;
        }

        for (const auto& frame : batch) {
            const auto parsed = ParseGuestMessage(frame);
            if (!parsed.has_value()) {
                result.failure = PtcFailure::Rpc;
                result.error = "坏帧: " + parsed.error();
                terminal = true;
                break;
            }
            const GuestMessage& message = *parsed;
            if (message.kind == GuestMessage::Kind::Hello) {
                if (got_hello || message.protocol != kProtocolVersion) {
                    result.failure = PtcFailure::Protocol;
                    result.error = message.protocol != kProtocolVersion
                                       ? ("协议版本不符: 脚本侧 " + std::to_string(message.protocol) + ", 宿主 " +
                                          std::to_string(kProtocolVersion))
                                       : "重复 hello 帧";
                    terminal = true;
                    break;
                }
                got_hello = true;
                result.python_version = message.python;
                continue;
            }
            if (!got_hello) {
                result.failure = PtcFailure::Protocol;
                result.error = "第一帧不是 hello";
                terminal = true;
                break;
            }
            if (message.kind == GuestMessage::Kind::Emit) {
                result.emit_value = message.value;
                continue;
            }
            if (message.kind == GuestMessage::Kind::Done) {
                result.captured_stdout = message.captured_stdout;
                // 脚本撞过调用数/并发墙但自己收口完毕:墙照记在账里(error
                // 留空),整轮按成功算——摘要在,拒绝过的调用在审计账上。
                if (result.failure == PtcFailure::LimitCalls || result.failure == PtcFailure::LimitConcurrency) {
                    result.failure = PtcFailure::None;
                    result.error.clear();
                }
                terminal = true;
                break;
            }
            if (message.kind == GuestMessage::Kind::Fail) {
                result.stage = message.stage;
                result.error = message.error;
                result.traceback = message.traceback;
                result.failure = PtcFailure::Runtime;
                if (message.stage == "syntax") {
                    result.failure = PtcFailure::Syntax;
                } else if (message.stage == "import") {
                    result.failure = PtcFailure::Import;
                } else if (message.stage == "guard") {
                    result.failure = PtcFailure::Guard;
                } else if (message.stage == "rpc") {
                    result.failure = PtcFailure::Rpc;
                }
                terminal = true;
                break;
            }
            // Call:先过两道数的墙,再进执行链。
            PtcCallRecord record;
            record.id = message.id;
            record.tool = message.tool;
            record.input = message.input;
            record.input_hash = StableHash(message.input);
            record.side_effect_class = "none";
            state.calls_received.fetch_add(1);
            if (static_cast<int>(result.calls.size()) >= options.limits.max_calls) {
                send_result(message.id, false, nlohmann::json::object(),
                            "调用数已达上限(" + std::to_string(options.limits.max_calls) + "),本次调用被拒");
                finish_call(std::move(record), false, "调用数上限(" + std::to_string(options.limits.max_calls) + ")",
                            0);
                result.failure = PtcFailure::LimitCalls;
                result.error = FailureText(PtcFailure::LimitCalls);
                continue;
            }
            const int outstanding = state.calls_received.load() - state.responses_sent.load();
            if (outstanding > options.limits.max_concurrency) {
                send_result(message.id, false, nlohmann::json::object(),
                            "并发调用数超上限(" + std::to_string(options.limits.max_concurrency) +
                                ",在飞 " + std::to_string(outstanding) + "),本次调用被拒");
                finish_call(std::move(record), false,
                            "并发数上限(" + std::to_string(options.limits.max_concurrency) + ")", 0);
                result.failure = PtcFailure::LimitConcurrency;
                result.error = FailureText(PtcFailure::LimitConcurrency);
                continue;
            }
            // Esc 取消链:还没开始的调用直接回"已取消",不再进执行链。
            if (options.cancel != nullptr && options.cancel->load()) {
                send_result(message.id, false, nlohmann::json::object(), "已取消(Esc): 本次调用未开始");
                finish_call(std::move(record), false, "取消(未开始)", 0);
                result.failure = PtcFailure::Cancelled;
                result.error = FailureText(PtcFailure::Cancelled);
                continue;
            }
            // 同参只读去重:同工具 + 入参 hash 已成功过的调用直接复用结果,
            // 不再烧一遍工具链(账上记 reused,不装新执行)。失败过的调用
            // 不缓存——工具层失败可能是暂态,重试是脚本的正当权利。
            if (options.reuse_identical_readonly_calls) {
                const auto cached = reuse_cache.find({record.tool, record.input_hash});
                if (cached != reuse_cache.end()) {
                    nlohmann::json value = nlohmann::json::object();
                    value["content"] = cached->second;
                    value["is_error"] = false;
                    send_result(message.id, true, value, "");
                    record.result_hash = StableHashText(cached->second);
                    record.reused = true;
                    finish_call(std::move(record), true, "", 0);
                    ++reused_calls;
                    continue;
                }
            }
            // 完整宿主链:schema 校验/PreToolUse/权限/执行/PostToolUse 都在
            // executor 里(PtcTool 装配),这里只记账。
            const auto call_started = Clock::now();
            tools::Tool::Result tool_result;
            try {
                tool_result = options.executor(message.tool, message.input);
            } catch (const std::exception& exc) {
                tool_result = tools::Tool::Result{std::string("宿主执行链异常: ") + exc.what(), true};
            }
            const int elapsed_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - call_started).count());
            nlohmann::json value = nlohmann::json::object();
            value["content"] = tool_result.content;
            value["is_error"] = tool_result.is_error;
            send_result(message.id, true, value, "");
            record.result_hash = StableHashText(tool_result.content);
            record.is_error = tool_result.is_error;
            if (options.reuse_identical_readonly_calls && !tool_result.is_error) {
                reuse_cache[{record.tool, record.input_hash}] = tool_result.content;
            }
            finish_call(std::move(record), true, tool_result.is_error ? "工具层失败" : "", elapsed_ms);
        }
        if (terminal) {
            break;
        }

        if (frames_failed) {
            std::lock_guard<std::mutex> lock(state.mutex);
            result.failure = PtcFailure::Rpc;
            result.error = "分帧失败: " + state.frames_error;
            break;
        }

        // 输出字节墙(双向合计)。
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (options.limits.output_bytes > 0 && state.bytes_in + state.bytes_out > options.limits.output_bytes) {
                result.failure = PtcFailure::LimitOutput;
                result.error = FailureText(PtcFailure::LimitOutput) + " (上限 " +
                               std::to_string(options.limits.output_bytes) + " 字节)";
                break;
            }
        }

        // Esc:未开始的调用在逐枚回绝(见 Call 分支),这里先给脚本发 abort
        // 让它收口(try/except 后 emit),留两秒宽限;宽限过了还没退,再杀树。
        if (options.cancel != nullptr && options.cancel->load()) {
            if (!abort_sent) {
                abort_sent = true;
                child.Write(EncodeFrame(BuildAbortPayload("cancelled by user")));
                cancel_deadline = Clock::now() + std::chrono::seconds(2);
                if (result.failure == PtcFailure::None) {
                    result.failure = PtcFailure::Cancelled;
                    result.error = FailureText(PtcFailure::Cancelled);
                }
            } else if (Clock::now() >= cancel_deadline) {
                break;
            }
        }

        // 墙钟。
        const auto now = Clock::now();
        if (options.limits.wall_clock_ms > 0 && now >= deadline) {
            result.failure = PtcFailure::LimitWallClock;
            result.error = FailureText(PtcFailure::LimitWallClock) + " (上限 " +
                           std::to_string(options.limits.wall_clock_ms) + " ms)";
            child.Write(EncodeFrame(BuildAbortPayload("wall clock limit")));
            break;
        }

        // 进程死了且再无新帧(300ms 静默)= EOF 收场。
        bool dead_and_quiet = false;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            dead_and_quiet = state.pending_frames.empty() &&
                             now - state.last_frame_at > std::chrono::milliseconds(300) && !child.IsAlive();
        }
        if (dead_and_quiet) {
            break;
        }
        // 已拒调用:给脚本一点时间收口(try/except 后 emit),但不等到底。
        if (result.failure == PtcFailure::LimitCalls || result.failure == PtcFailure::LimitConcurrency) {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (Clock::now() - state.last_frame_at > std::chrono::seconds(2)) {
                break;
            }
        }

        std::unique_lock<std::mutex> lock(state.mutex);
        state.cv.wait_until(lock, now + std::chrono::milliseconds(250),
                            [&state] { return !state.pending_frames.empty(); });
    }

    // ---- 4. 收场:关停、归因、清场 ----
    child.Shutdown(2000);
    result.exit_code = child.exit_code();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (result.failure != PtcFailure::None && !state.stderr_text.empty()) {
            std::string tail = state.stderr_text.substr(state.stderr_text.size() > 2000 ? state.stderr_text.size() - 2000 : 0);
            result.error += " [stderr] " + tail;
        }
    }

    // 无终局帧的死法:按资源账归因(CPU/内存墙),归因不出算沙箱终止。
    if (result.failure == PtcFailure::None && result.emit_value.is_null()) {
        const auto usage = child.ResourceUsageSnapshot();
        const auto cpu_limit_100ns = static_cast<std::uint64_t>(options.limits.cpu_ms) * 10000ULL;
        if (options.use_os_sandbox && options.limits.cpu_ms > 0 && usage.cpu_100ns >= cpu_limit_100ns) {
            result.failure = PtcFailure::LimitCpu;
            result.error = FailureText(PtcFailure::LimitCpu) + " (上限 " + std::to_string(options.limits.cpu_ms) +
                           " ms)";
        } else if (options.use_os_sandbox && options.limits.memory_bytes > 0 &&
                   usage.peak_memory_bytes >= options.limits.memory_bytes) {
            result.failure = PtcFailure::LimitMemory;
            result.error = FailureText(PtcFailure::LimitMemory);
        } else if (result.exit_code < 0) {
            result.failure = PtcFailure::Sandbox;
            result.error = FailureText(PtcFailure::Sandbox) + " (信号 " + std::to_string(-result.exit_code) + ")";
        } else {
            result.failure = PtcFailure::Sandbox;
            result.error = FailureText(PtcFailure::Sandbox) + " (无终局帧, 退出码 " +
                           std::to_string(result.exit_code) + ")";
        }
    }
    // 到了 done 且有 emit 才算成(没 emit 的死法已在上面归因成 Protocol/Sandbox)。
    result.ok = result.failure == PtcFailure::None && !result.emit_value.is_null();
    result.elapsed_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count());
    cleanup();
    return result;
}

}  // namespace lubancode::ptc
