// runtime committed wake 窄口(端云协同可观测架构与 Telemetry 插件设计单
// §25.3,实施分期 T1"runtime committed wake 窄口")。
//
// 只投"哪条 stream 有新账",不带事实正文(§14.1:wake 只说有新账;丢了
// 不要紧,周期扫描或 session close 会补)。Notify 必须 noexcept、非阻塞:
// 实现只许加锁入集合 + notify,投不进只加 missed-wake 原子计数,由
// Projector 后续扫描补账——绝不能反压 Recorder 或 Agent Loop(§17.2)。
//
// 依赖方向(§25.2):runtime 只认本件这只窄口;trajectory 纯库永不 include
// 它,装配层负责把 TelemetryService 挂到 TrajectorySessionLedger 侧。
#pragma once

#include <string>

namespace lubancode::telemetry {

// 一枚提交唤醒:workspace/session/stream 三件钉死账的位置。stream_id 是
// session 目录内的相对路径("main.jsonl"、"subagents/<run_id>.jsonl"),
// 与 cursor 的 stream 字段(§14.2)同一口径。
struct CommitWake {
    std::string workspace_key;
    std::string session_id;
    std::string stream_id;
};

class CommitObserver {
public:
    virtual ~CommitObserver() = default;
    virtual void Notify(const CommitWake& wake) noexcept = 0;
};

}  // namespace lubancode::telemetry
