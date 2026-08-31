// Workspace 统一存储 P0-0:冻结合同(纯常量,不接线)。
//
// 本头只放 P0-0 冻结的 schema 名、版本号、seed 前缀与稳定错误码。后续
// 波次(P0-1/P0-2/P0-3/P0-5)实现时 #include 这些常量,不许各写各的魔法串。
// P0-0 不新增任何符号链接、不接任何生产调用点——单子《Workspace 统一存储、
// 旧 Session 退场与分级 Memory 迁移》§P0-0。
//
// 合同正文(schema 样例、字段中英对照、目录布局)见
// docs/development/workspace-storage-v2/P0-0-contracts.md;本头与文档同改,
// 漂移即 bug。
#pragma once

#include <string_view>

namespace lubancode::workspace::contracts {

// ---------------------------------------------------------------------------
// schema 标识与版本(workspace v2 一棵树)
// ---------------------------------------------------------------------------

// workspace.json 的 schema 名与版本。v2 起 manifest 双键定形:
//   {"schema": "lubancode.workspace", "version": 2, ...}
// 旧 trajectories/workspaces/*/workspace.json 是 {"schema_version": 1, ...},
// 只许迁移器读,生产 reader 见 version>2 一律拒(错误码 schema.unsupported_version)。
inline constexpr std::string_view kWorkspaceSchemaName = "lubancode.workspace";
inline constexpr int kWorkspaceSchemaVersion = 2;
inline constexpr int kWorkspaceMinReaderVersion = 2;

// session.json(轨迹场 manifest)。v1 是 trajectories 时代的形状;新根
// ~/.lubancode/workspaces 下读到 v1 = 旧档搬错了家,doctor 报 corrupt。
inline constexpr int kSessionManifestSchemaVersion = 2;
inline constexpr int kSessionManifestMinReaderVersion = 2;

// 事件信封 schema(沿用 trajectory v1 合同,版本号不重起):
// envelope.schema_version 与 session.json 的 event_schema_version 钉死。
inline constexpr int kEnvelopeSchemaVersion = 1;

// 迁移回执(migrations/storage-v2/<operation_id>/{intent,progress,result}.json)。
inline constexpr std::string_view kMigrationSchemaName = "lubancode.storage-migration";
inline constexpr int kMigrationSchemaVersion = 1;
inline constexpr int kMigrationMinReaderVersion = 1;

// memory recall snapshot(typed trajectory event context.injected 的 payload
// 与 artifact 双形态共用这份版本)。
inline constexpr std::string_view kMemoryRecallSchemaName = "lubancode.memory-recall";
inline constexpr int kMemoryRecallSchemaVersion = 1;
inline constexpr int kMemoryRecallMinReaderVersion = 1;

// workspace_key seed 前缀(身份裁决 §4.3;三选一,不新增)。
inline constexpr std::string_view kSeedPrefixGit = "git:";        // + 规范绝对 common git dir
inline constexpr std::string_view kSeedPrefixMarker = "marker:";  // + marker.workspace_id
inline constexpr std::string_view kSeedPrefixPath = "path:";      // + 规范绝对 project_root

// identity_kind 四值封闭(§4.1;不加第五种)。
inline constexpr std::string_view kIdentityKindGitCommon = "git_common";
inline constexpr std::string_view kIdentityKindExplicitMarker = "explicit_marker";
inline constexpr std::string_view kIdentityKindConfigRoot = "config_root";
inline constexpr std::string_view kIdentityKindCwdFallback = "cwd_fallback";

// session manifest 的 start_reason 枚举(v2 冻结;process_launch/clear/resume
// 沿用现状,legacy_import 是迁移器专用)。
inline constexpr std::string_view kStartReasonProcessLaunch = "process_launch";
inline constexpr std::string_view kStartReasonClear = "clear";
inline constexpr std::string_view kStartReasonResume = "resume";
inline constexpr std::string_view kStartReasonLegacyImport = "legacy_import";

// 旧 Session 迁入的子账缺口标注(§7.2:旧主账只有 agent 最终回话,不得
// 伪造成完整子代理 Journal)。
inline constexpr std::string_view kSubagentDetailUnavailableLegacy = "unavailable_legacy";

// 训练投影对迁移来源的固定策略(§7.1 第 5 条)。
inline constexpr std::string_view kLegacyTrainingPolicy = "exclude";
inline constexpr std::string_view kLegacyFidelityLevel = "partial";

// ---------------------------------------------------------------------------
// 稳定错误码(§9.2 失败合同 + §8.2 命令错误分型)
//
// 风格沿用现有点分制 <域>.<原因>(schema.*、resume.*、verify.* 同款)。
// 本批只新增下列域;既有码(见 contracts 文档附录)不重编。错误码一旦发布
// 即冻结:只加不改不删。
// ---------------------------------------------------------------------------

// 身份裁决(P0-1)。
inline constexpr std::string_view kErrIdentityNoBoundary = "identity.no_boundary";
inline constexpr std::string_view kErrIdentityKeyMismatch = "identity.key_mismatch";
inline constexpr std::string_view kErrIdentityPathInvalid = "identity.path_invalid";

// workspace 开合与锁(P0-1/P0-2)。
inline constexpr std::string_view kErrWorkspaceNotFound = "workspace.not_found";
inline constexpr std::string_view kErrWorkspaceOpenFailed = "workspace.open_failed";
inline constexpr std::string_view kErrWorkspaceLocked = "workspace.locked";
inline constexpr std::string_view kErrWorkspacePermissionDenied = "workspace.permission_denied";
inline constexpr std::string_view kErrWorkspaceDiskFull = "workspace.disk_full";

// session 读写(P0-2)。
inline constexpr std::string_view kErrSessionNotFound = "session.not_found";
inline constexpr std::string_view kErrSessionOpenFailed = "session.open_failed";
inline constexpr std::string_view kErrSessionLocked = "session.locked";
inline constexpr std::string_view kErrSessionJournalCorrupt = "session.journal_corrupt";
inline constexpr std::string_view kErrSessionParentEdgeBroken = "session.parent_edge_broken";
inline constexpr std::string_view kErrSessionWriteFailed = "session.write_failed";
inline constexpr std::string_view kErrSessionMigrationPending = "session.migration_pending";

// memory(P0-3/P0-4)。
inline constexpr std::string_view kErrMemorySaveFailed = "memory.save_failed";
inline constexpr std::string_view kErrMemoryRecallSnapshotFailed = "memory.recall_snapshot_failed";
inline constexpr std::string_view kErrMemoryGlobalUnauthorized = "memory.global_unauthorized";
inline constexpr std::string_view kErrMemoryJobFailed = "memory.job_failed";

// 存储迁移(P0-5)。
inline constexpr std::string_view kErrMigrationIntentExists = "migration.intent_exists";
inline constexpr std::string_view kErrMigrationResultExists = "migration.result_exists";
inline constexpr std::string_view kErrMigrationSourceShaMismatch = "migration.source_sha_mismatch";
inline constexpr std::string_view kErrMigrationSourceUnreadable = "migration.source_unreadable";
inline constexpr std::string_view kErrMigrationTargetWriteFailed = "migration.target_write_failed";
inline constexpr std::string_view kErrMigrationInterrupted = "migration.interrupted";
inline constexpr std::string_view kErrMigrationDeleteUnverified = "migration.delete_unverified";

}  // namespace lubancode::workspace::contracts
