// PTC 受限 runner(P0 核心):起沙箱 Python 子进程、喂脚本、按 framed RPC
// 协议把 stub 调用交回宿主执行、收 emit 摘要,全程记审计账。
//
// 五道上限(规格"安全边界"第 5 条),撞线指名是哪道墙:
//   1. 墙钟(wall_clock_ms)——宿主看门狗,超时杀进程树;
//   2. CPU(cpu_ms)——Windows Job PROCESS_TIME / POSIX RLIMIT_CPU;
//   3. 内存(memory_bytes)——Job PROCESS_MEMORY / RLIMIT_AS;
//   4. 输出字节(output_bytes)——RPC 帧负载双向合计 + stderr;
//   5. 调用数(max_calls)与并发数(max_concurrency)——宿主逐帧记数,
//      在飞未答的调用超过并发窗,按并发墙拒。
//
// 取消链(Esc):先对未开始的调用回"已取消"(脚本侧 ToolCallError),再
// 让在跑工具自然收尾(与 JSON 路同一语义:已开始的调用结果照常入账),
// 最后杀脚本进程树(ChildProcess::Shutdown 关 job/进程组,后代不留)。
//
// runner 不认 ToolRegistry/hooks/权限——那些在 PtcCallExecutor 里,由
// PtcTool 装配成与 JSON 工具完全相同的一条链(schema 校验/PreToolUse/
// PermissionRequest/执行/PostToolUse/审计)。

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "tools/tool.hpp"

namespace lubancode::ptc {

// 五道上限的运行档。0 = 那道墙不设(只用于单测;生产路径 PtcTool 永远全设)。
struct PtcLimits {
    int wall_clock_ms = 30000;
    int cpu_ms = 20000;
    std::size_t memory_bytes = 512 * 1024 * 1024;
    std::size_t output_bytes = 8 * 1024 * 1024;
    int max_calls = 100;
    int max_concurrency = 8;
};

// 撞了哪道墙/败在哪个环节。
enum class PtcFailure {
    None,        // 成功(或脚本业务失败但有账)
    Spawn,       // python 起不来/找不到
    Syntax,      // 脚本语法错(可考虑回退 JSON:零调用发生)
    Import,      // import 错(同上)
    Runtime,     // 脚本运行时错
    Guard,       // 脚本试图越护栏(import 白名单拒)
    Rpc,         // RPC 协议错(熔断器认)
    LimitWallClock,
    LimitCpu,
    LimitMemory,
    LimitOutput,
    LimitCalls,
    LimitConcurrency,
    Cancelled,   // Esc 取消链收场
    Sandbox,     // 沙箱杀了进程,具体墙归因不出
    Protocol,    // 帧协议坏/流程不对(没 hello、没 done)
};

// 单枚 stub 调用的审计账(规格"副作用与回退"节的调用账字段)。
struct PtcCallRecord {
    std::uint64_t id = 0;
    std::string tool;
    nlohmann::json input;          // 原始入参(审计层用;语义层只进 hash)
    std::string input_hash;        // 规范化入参 hash(去重键的一部分)
    std::string result_hash;       // 结果内容 hash
    bool ok = false;               // 工具链放行且执行成功
    bool is_error = false;         // 工具层失败(文件不存在等)
    bool reused = false;           // 同参只读去重命中:没重跑,复用了上一次结果
    std::string error;             // 拒绝/失败说明
    int elapsed_ms = 0;
    std::string side_effect_class; // "none"(只读)/"write"——入选集只读恒 none
};

// 一次 PTC 运行的完整结果。
struct PtcRunResult {
    bool ok = false;
    PtcFailure failure = PtcFailure::None;
    std::string error;             // 人读错误(撞线指名哪道墙)
    std::string traceback;         // 脚本侧 traceback(截尾)
    std::string stage;             // 脚本侧自报的 stage
    nlohmann::json emit_value;     // 脚本 emit 的最终摘要(成功时有)
    std::vector<PtcCallRecord> calls;
    std::string ptc_run_id;        // 如 ptc-1a2b3c4d
    std::string script_hash;
    std::string captured_stdout;   // 脚本 print 捕获(截尾)
    std::string sandbox_grade;     // "windows-job+restricted-token" / "posix-rlimit" / "none"
    int exit_code = -1;
    int elapsed_ms = 0;
    std::string python_version;    // hello 帧报的版本

    // 回退判定(规格"回退规矩"):零调用发生时 syntax/import 可以自动回 JSON。
    bool ZeroCallsHappened() const { return calls.empty(); }
    // 已发生副作用(本版入选集只读,恒 false;字段留给 P3 写工具)。
    bool HasSideEffects() const;
    // 只读已发生(可复用结果再补步骤)。
    bool HasReadOnlyCalls() const;
};

// 宿主侧执行一枚 stub 调用的完整链。runner 只管协议与账,链在调用方。
using PtcCallExecutor = std::function<tools::Tool::Result(const std::string& name, const nlohmann::json& input)>;

class PtcRunner {
public:
    struct Options {
        std::string python_cmd;              // 解释器(python/python3/绝对路径)
        PtcLimits limits;
        PtcCallExecutor executor;            // 必填:没有执行链的 runner 没意义
        const std::atomic<bool>* cancel = nullptr;  // Esc 取消链
        bool use_os_sandbox = true;          // Windows/POSIX 落资源墙;false 只靠 Python 护栏(测试用)
        bool restricted_token = true;        // Windows 受限 token(起不成就降级,记档)
        std::string work_dir;                // 临时目录父目录(空 = 系统临时目录)
        // 同参只读去重(规格"历史与压缩"节):同工具 + 规范化入参 hash 的
        // 只读调用在本轮内复用上一次结果,不再烧一遍工具链。账上记
        // reused=true,不装成新执行。默认开(PtcTool 只读入选集)。
        bool reuse_identical_readonly_calls = true;
    };

    // 跑一段脚本。stub_module_python 是生成好的 luban_tools.py 全文。
    // script 是模型写的 Python 正文。全过程同步,返回完整账。
    static PtcRunResult Run(const std::string& script, const std::string& stub_module_python, Options options);

    // 生成一枚运行 id(ptc-<8 位随机十六进制>)。导出给 PtcTool 复用。
    static std::string NewRunId();

    // 稳定 hash(规范化 JSON dump 后 FNV-1a),入参/结果 hash 用。
    static std::string StableHash(const nlohmann::json& value);
    static std::string StableHashText(const std::string& text);

    // 撞线/失败的人读名字(测试与 UI 共用)。
    static std::string FailureText(PtcFailure failure);
};

// 当前平台的沙箱档位描述(画像硬条件 #1 用)。返回 "none" 表示没有可靠
// 沙箱(POSIX 的 rlimit 只限资源,不算"可靠沙箱",默认禁 PTC)。
std::string PlatformSandboxGrade(bool allow_posix_rlimit_fallback);

}  // namespace lubancode::ptc
