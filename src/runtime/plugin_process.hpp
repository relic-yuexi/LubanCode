// process 运行时(plugins 单第 2 步):manifest 里 runtime.kind=process 的
// 执行器。每次 tool call 起一只短命进程——stdin 恰好一份 JSON 请求、写完
// 即关;stdout 是结果专线(恰好一份 JSON 响应);stderr 只攒日志。无握手、
// 无常驻 server、无 JSON-RPC。
//
// 进程边界(单子「进程执行与资源边界」):
//   - 无 shell:Windows argv 直传(CreateProcess quoting helper 在
//     platform/process_win.cpp),POSIX execve 路子(platform/process_posix.cpp
//     现成基建);参数里的引号/空格/;&| 不可能变成命令。
//   - 三管异步排水:ChildProcess 自带 stdout/stderr 两条读线程,子进程
//     写满哪条都不会死锁。
//   - 超时先温和终止、过 grace 杀整棵进程树(Windows Job Object/POSIX
//     process group,ChildProcess::Shutdown 一并管了);ESC 同一取消路。
//   - 输入/stdout/stderr 各设字节帽:输入超帽在发之前就拒;输出超帽在
//     读线程里发现即杀,不等进程自己跑完。
//   - env 最小继承集:PATH 与必要系统变量 + manifest allowlist 点名的
//     变量;宿主整份环境(连 API key)一概不递。
//   - cwd 缺省项目根(调用方传进来)。
//
// 终态唯一:成功/插件自报失败/非零退出/信号/超时/取消/坏 UTF-8/坏 JSON/
// call_id 不合/输出超限/未知 content 各有唯一 PluginErrorCode,ItemCompleted
// 只落一笔(上层 Tool::Result 一进一出)。
#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "runtime/plugin_contract.hpp"

namespace lubancode::runtime {

// 一次 process 调用的执行选项与上限。0 = 不设(生产路径不该用 0 的
// timeout;stdout/stderr 帽给 0 时用默认值)。
struct ProcessCallLimits {
    int timeout_ms = 30000;                 // 墙钟;超时温和终止 + grace 杀树
    std::size_t stdin_cap_bytes = 4 * 1024 * 1024;   // 请求帧字节帽(发前拒)
    std::size_t stdout_cap_bytes = 4 * 1024 * 1024;  // 响应字节帽(读中发现即杀)
    std::size_t stderr_cap_bytes = 256 * 1024;       // 日志字节帽(截断保留前段,不杀)
};

// 一次调用的完整终态。code 是唯一口径;detail 是人话(拼给模型/日志);
// stderr_tail 是插件日志的尾巴(诊断用,不进模型);exit_code 在进程
// 退出后有效(-1 = 没到收尸那步)。
struct ProcessCallOutcome {
    PluginErrorCode code = PluginErrorCode::Ok;
    std::string text;        // code==Ok 时:响应文本(送模型)
    nlohmann::json structured;  // code==Ok 时:响应 structured(可选,前端用)
    std::string plugin_error_code;    // code==PluginReportedError 时:插件自报的码
    std::string detail;
    std::string stderr_tail;
    int exit_code = -1;
    bool ok() const { return code == PluginErrorCode::Ok || code == PluginErrorCode::PluginReportedError; }
};

// 起进程、跑完一次调用、收终态。全过程同步(工具执行本来就是顺序的),
// 返回唯一终态——绝不抛异常、绝不吊死(超时/取消各有一条有界收尾路)。
//
//   manifest:已解析校验的插件清单(argv 从这里来)。
//   request:协议请求帧的字段(SerializeRequest 在内部拼,写完 stdin 即关)。
//   cwd_utf8:子进程工作目录(缺省项目根,由调用方传)。
//   cancel:ESC 取消链;置位走与超时同一条收尾路,分型 Cancelled。
//   limits:上限;timeout_ms 会与 manifest.timeout_ms 取较小者(manifest
//     是插件作者给的墙,不许运行时悄悄放宽)。
//   stderr_tail_out:可选,接收 stderr 全量(已截到帽)——测试断言用;
//   production 不需要就传 nullptr,日志只进 detail。
ProcessCallOutcome RunProcessToolCall(const PluginManifest& manifest,
                                      const plugin_protocol::ProcessRequest& request, const std::string& cwd_utf8,
                                      const std::atomic<bool>* cancel, const ProcessCallLimits& limits,
                                      std::string* stderr_tail_out = nullptr);

}  // namespace lubancode::runtime
