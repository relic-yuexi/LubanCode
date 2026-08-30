// Trajectory 目录制(P0 新轨迹记录单 §三):workspace -> session 两层。
//
//   <trajectories_root>/workspaces/<workspace_key>/
//     workspace.json  lifecycle/  tombstones/  sessions/<session_id>/...
//
// session 目录树照 §3.1 全量占位;session.json 只存静态材料,写法是
// 临时文件 + 原子 rename(§3.3)。本单只做 preparing/running 两态写入,
// 完整生命周期状态机归 SessionManager(后续批次)。
//
// scoped JSONL 创建器照 §3.10:create-new 原子占位,文件为空即返回;
// 次序(父 dispatched 先 durable,子 run.started 后)由调用方掌管。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::trajectory {

// ---------------------------------------------------------------------------
// workspace_key(§3.2)
// ---------------------------------------------------------------------------

// 规范化:绝对路径 + lexically_normal + 正斜杠 + 去尾斜杠 + Windows 盘符
// 小写。跨平台同一份规则,hash 输入先过这道。
std::string NormalizeRootPathText(const std::filesystem::path& root);

// workspace_key = <root-basename>-<first12(SHA256(normalized-root))>。
std::string ComputeWorkspaceKey(const std::filesystem::path& root);

// 向上找 .git 定仓库根;不在 Git 仓库内给 nullopt(调用方退回启动 cwd)。
std::optional<std::filesystem::path> FindWorkspaceRoot(const std::filesystem::path& cwd);

// ---------------------------------------------------------------------------
// session_id(§3.3)
// ---------------------------------------------------------------------------

// YYYYMMDD-HHMMSS-XXXXXX:时间给人看,尾 6 位防撞。random6 由调用方注入
// (大写字母数字);生成不依赖隐藏全局态,测试可复现。
std::string GenerateSessionId(int year, int month, int day, int hour, int minute, int second,
                              std::string_view random6);

// ---------------------------------------------------------------------------
// session.json(§3.3)
// ---------------------------------------------------------------------------

struct SessionManifest {
    int schema_version = 1;
    std::string workspace_key;
    std::string session_id;
    std::string launch_cwd;                 // UTF-8 文本
    std::string main_run_id;
    std::string start_reason = "process_launch";
    std::optional<std::string> previous_session_id;
    std::string status = "preparing";  // 本单只写 preparing/running
    std::int64_t created_at_ms = 0;
    std::string lubancode_version;

    nlohmann::json ToJson() const;
    static std::optional<SessionManifest> FromJson(const nlohmann::json& json);
};

// 原子写:同目录临时文件 + rename(复用 platform::ReplaceFileAtomically,
// Windows 覆盖目标,POSIX rename 原子)。
std::expected<void, std::string> WriteSessionJsonAtomic(const std::filesystem::path& session_dir,
                                                        const SessionManifest& manifest);
std::optional<SessionManifest> ReadSessionJson(const std::filesystem::path& session_dir);

// ---------------------------------------------------------------------------
// 目录创建器
// ---------------------------------------------------------------------------

class TrajectoryDirectory {
public:
    TrajectoryDirectory() = default;

    // 建 workspace 层:workspaces/<key>/ + workspace.json + sessions/ +
    // lifecycle/ + tombstones/。已存在照旧(workspace.json 不覆盖——首次
    // 创建时间等材料以旧账为准)。
    static std::expected<TrajectoryDirectory, std::string> CreateWorkspace(
        const std::filesystem::path& trajectories_root, const std::filesystem::path& workspace_root,
        const std::string& readable_name, std::int64_t created_at_ms);

    // 建 session 层:§3.1 全目录树 + session.json(status=preparing)。
    // session_id 已存在即失败(绝不复用,§3.3)。
    static std::expected<TrajectoryDirectory, std::string> CreateSession(
        const std::filesystem::path& workspaces_root, const std::string& workspace_key,
        const SessionManifest& manifest);

    // 认领既有 session 目录(恢复器/管理操作用):只回填两段路径,不建
    // 目录、不验内容——写入合法性由锁与 Journal 状态机把门。
    static TrajectoryDirectory OpenExisting(const std::filesystem::path& session_dir);

    // ---- scoped JSONL 创建器(§3.10 create-new 占位) ----
    // main.jsonl。
    std::expected<std::filesystem::path, std::string> ReserveMainStream() const;
    // subagents/<agent_run_id>.jsonl(嵌套 subagent 同目录,§3.5)。
    std::expected<std::filesystem::path, std::string> ReserveSubagentStream(
        const std::string& agent_run_id) const;
    // workflows/<workflow_run_id>/{workflow.jsonl, checkpoints/, nodes/}。
    std::expected<std::filesystem::path, std::string> ReserveWorkflowRun(
        const std::string& workflow_run_id) const;
    // workflows/<workflow_run_id>/nodes/<node_run_id>.jsonl(重试新开文件,
    // attempt 不共写,§3.6)。
    std::expected<std::filesystem::path, std::string> ReserveWorkflowNodeStream(
        const std::string& workflow_run_id, const std::string& node_run_id) const;

    const std::filesystem::path& workspace_dir() const { return workspace_dir_; }
    const std::filesystem::path& session_dir() const { return session_dir_; }
    std::filesystem::path artifacts_root() const { return session_dir_ / "artifacts"; }
    std::filesystem::path main_stream_path() const { return session_dir_ / "main.jsonl"; }

private:
    std::filesystem::path workspace_dir_;
    std::filesystem::path session_dir_;
};

}  // namespace lubancode::trajectory
