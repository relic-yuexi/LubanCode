// Trajectory 目录制(P0 新轨迹记录单 §三):workspace -> session 两层。
//
//   <workspaces_root>/<workspace_key>/
//     workspace.json  lifecycle/  tombstones/  sessions/<session_id>/...
//
// P0-2(Workspace 统一存储)起 workspaces_root 是唯一项目持久化根
// `~/.lubancode/workspaces`(从 `~/.lubancode/trajectories/workspaces`
// 迁来;"trajectories" 生产目录退场,旧根零读零写)。
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

#include "workspace/identity.hpp"

namespace lubancode::trajectory {

// ---------------------------------------------------------------------------
// workspace_key(P0-1 起由 workspace::ResolveWorkspaceIdentity 唯一裁决:
// Git common dir→marker→config→cwd 四级,SHA256 前 16 位 + seed 前缀。
// trajectory 只吃结果,不再各算各的 key。)
// ---------------------------------------------------------------------------

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
    // P0-2(存储 v2 合同 §三):新根下 session.json 一律 v2。v1 只许旧根
    // 旧档与迁移器输入侧出现;新根读到 v1 即"旧档搬错家",reader 按坏档
    // 对待。v2 与 v1 字段同构,新增键(subagent_detail/training_policy)
    // 只有 legacy_import 迁移场才写(P0-5)。
    int schema_version = 2;
    // 本 session 各 stream 的 event schema major(Token 账本单 §6.1.1):
    // v2 = usage 走 model.usage.recorded。manifest 钉死 major,旧 manifest
    // 没写这键按 v1 读。
    int event_schema_version = 1;
    std::string workspace_key;
    std::string session_id;
    std::string launch_cwd;                 // UTF-8 文本
    std::string main_run_id;
    // main run 的种类(单发轨迹断档单):默认 main_session;one_shot 一场
    // 写 one_shot。旧 manifest 没这键按默认读——resume 候选与管理面据此
    // 认单发场(单发语义不续,审计可读)。
    std::string run_kind = "main_session";
    std::string start_reason = "process_launch";
    std::optional<std::string> previous_session_id;
    std::string status = "preparing";  // 本单只写 preparing/running
    std::int64_t created_at_ms = 0;
    std::string lubancode_version;
    // 存储 v2 合同 §三(P0-5 接线):仅 start_reason=legacy_import 场必填。
    // subagent_detail 恒 unavailable_legacy(旧主账只有 agent 最终回话,
    // 不伪造子 Journal);training_policy 恒 exclude(复现等级不高于
    // partial)。空 = 非迁移场,两键不落盘。
    std::string subagent_detail;
    std::string training_policy;

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

    // 建 workspace 层:workspaces_root/<identity.workspace_key>/ +
    // workspace.json(v2 manifest,由 workspace::OpenOrRegisterWorkspace 首仓
    // 原子写/开仓登记)+ sessions/ + lifecycle/ + tombstones/。已存在的
    // manifest 只更新 last_opened 与 checkout 登记,首次创建时间以旧账为准。
    static std::expected<TrajectoryDirectory, std::string> CreateWorkspace(
        const std::filesystem::path& workspaces_root, const workspace::WorkspaceIdentity& identity,
        std::int64_t now_ms);

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
