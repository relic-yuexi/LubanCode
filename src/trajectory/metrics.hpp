// 只读聚合诊断(P0 新轨迹记录单 §13.1,P0-4):`/doctor trajectory` 与
// `trajectory inspect` 共用的账本折叠口。
//
// §13.1 明说"普通状态查询只读,不为看面板再造一套账"——本件不持有任何
// 运行期状态、不设计数器、不常驻;每次调用都现扫 session.json + 各 JSONL
// + 磁盘余量,现折现报,与 `trajectory verify`/`usage` 同一口径的只读
// 扫描,只是把结果拼成人看得懂的一张表。
//
// 队列高水位与"最近 I/O 错误"这两项活跃期状态,trajectory 纯库不持有
// (那是 runtime::TrajectoryTurnBridge::recent_errors() 一类的运行时账),
// 由调用方(runtime/cli 装配层)采好递进来;本件只管折叠磁盘上的可证事实。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "trajectory/journal.hpp"
#include "trajectory/session_manager.hpp"
#include "trajectory/usage_gc.hpp"

namespace lubancode::trajectory {

struct StreamHealth {
    std::string label;              // "main" | "agent:<id>" | "workflow:<id>" | "node:<id>/<attempt>"
    std::filesystem::path path;
    bool exists = false;
    bool run_terminal = false;
    JournalVerifyReport verify;     // ok/truncated_tail/events/hash 等(journal.hpp)
};

struct SessionDoctorReport {
    std::string session_id;
    bool manifest_readable = false;
    SessionStatus status = SessionStatus::Preparing;
    std::vector<StreamHealth> streams;
    SessionCapacityUsage capacity;
    std::uint64_t unterminated_stream_count = 0;
};

// 扫一场 session 全部 stream(main + subagents/*.jsonl + workflows/*/
// workflow.jsonl + workflows/*/nodes/*.jsonl + goals/*.jsonl + loops/*.jsonl)
// 逐份验链,并入容量账(§12.2 四笔)。纯只读,不调模型不跑工具。
SessionDoctorReport BuildSessionDoctorReport(const std::filesystem::path& session_dir);

struct WorkspaceDoctorReport {
    std::string workspace_key;
    std::filesystem::path trajectories_root;
    bool disk_space_known = false;
    std::uint64_t disk_free_bytes = 0;
    std::uint64_t disk_total_bytes = 0;
    std::optional<SessionDoctorReport> active_session;
    std::vector<std::string> recent_errors;  // 调用方递入(§13.1"最近 I/O 错误")
    std::uint64_t queue_high_water_mark = 1;  // 单写者同步提交:恒 <=1(见头注)
    // P0-1:workspace v2 manifest 对账(key 与算法重算逐字比,不合即
    // identity.key_mismatch;版本超限即 schema.unsupported_version)。
    std::vector<std::string> manifest_issues;
    std::vector<std::string> notes;
};

// active_session_id 空 = 没有活动 session(只报磁盘与 workspace 概况)。
WorkspaceDoctorReport BuildWorkspaceDoctorReport(const std::filesystem::path& trajectories_root,
                                                 const std::filesystem::path& workspace_dir,
                                                 const std::string& workspace_key,
                                                 const std::optional<std::string>& active_session_id,
                                                 std::vector<std::string> recent_errors);

// 人话渲染(`/doctor trajectory` 与 CLI `trajectory doctor` 共用格式)。
std::vector<std::string> FormatWorkspaceDoctorReport(const WorkspaceDoctorReport& report);

// 磁盘 reserve 检查(§12.2/§13):目标目录所在文件系统的可用字节数低于
// reserve_bytes 时返回 false——写盘前调用方据此拒绝新副作用与大模型请求,
// 报告 storage_exhausted(§12.2),不可跑完工具才悄悄丢结果。目录不存在或
// 查询失败时保守判"空间不足"(宁可拒写,不误判有空间)。
bool HasDiskReserve(const std::filesystem::path& target_dir, std::uint64_t reserve_bytes);

}  // namespace lubancode::trajectory
