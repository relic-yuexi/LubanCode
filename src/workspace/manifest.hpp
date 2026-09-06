// workspace v2 manifest(`workspace.json`)的读写、checkout 登记与 doctor
// 对账(P0-1)。schema/字段合同冻结在 docs/development/workspace-storage-v2/
// P0-0-contracts.md §二与 src/workspace/storage_contracts.hpp;本件与合同
// 同改,漂移即 bug。
//
// 硬规矩:
//   - 首次开仓原子写(tmp + rename,形制沿用 trajectory/directory.cpp)。
//   - reader 见 version > 2 一律 schema.unsupported_version 拒读,不猜。
//   - manifest.workspace_key 与算法重算不合即 identity.key_mismatch 隔离
//     + doctor,不自动改名合并。
//   - checkouts[] 只是可重建登记,不是身份源;路径搬家不凭同名目录自动并账。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "workspace/identity.hpp"

namespace lubancode::workspace {

struct WorkspaceCheckout {
    std::string root;  // UTF-8 文本,正斜杠
    std::int64_t first_seen_at_ms = 0;
    std::int64_t last_seen_at_ms = 0;
};

struct WorkspaceManifest {
    std::string workspace_key;
    std::string display_name;
    std::string identity_kind;  // 四值封闭
    std::string identity_root;  // UTF-8 文本
    std::int64_t created_at_ms = 0;
    std::int64_t last_opened_at_ms = 0;
    std::vector<WorkspaceCheckout> checkouts;
    // migrated_from 只有 P0-5 迁移器写;读侧保留原样透传(不再改写)。
    std::optional<nlohmann::json> migrated_from;

    nlohmann::json ToJson() const;
    // 只认 version<=2 的 v2 形状;version>2 由 ReadWorkspaceManifest 先拦,
    // 这里不猜。缺键按合同默认兜。
    static std::optional<WorkspaceManifest> FromJson(const nlohmann::json& json);
};

// 读 workspace.json。missing = 文件不存在;unsupported_version/corrupt 带
// 稳定错误码(contracts::kErr*),文本说明补给人看。
struct ManifestRead {
    enum class Status { Ok, Missing, UnsupportedVersion, Corrupt };
    Status status = Status::Missing;
    std::string error_code;  // schema.unsupported_version | schema.missing_field
    std::string error_text;
    WorkspaceManifest manifest;  // status==Ok 时有效
};
ManifestRead ReadWorkspaceManifest(const std::filesystem::path& workspace_dir);

// 原子写(tmp + rename)。目录已存在 workspace.json 时整份替换(checkout
// 登记/last_opened 的更新也走这里)。
std::expected<void, std::string> WriteWorkspaceManifestAtomic(const std::filesystem::path& workspace_dir,
                                                              const WorkspaceManifest& manifest);

// 开仓或对账登记(P0-1 装配层的统一口;账本制起目录名走门牌,不推 key):
//   - 目录/manifest 不存在:按账本查门(账本制三步:查账→miss 生门牌→
//     开房记账),建目录,首仓 v2 原子写(created_at=now,checkouts=[当前
//     checkout]),记账进 workspaces/index.json。
//   - 已存在:版本协商(>2 拒)、key 对账(与 identity 重算不合即
//     identity.key_mismatch,不自动改名),过了更新 last_opened_at_ms 并
//     upsert checkout(按规范化 root 匹配;同 root 只更新 last_seen)。
// created_out 非空时回填是否首仓;workspace_dir_out 非空时回填实际房门
// (门牌目录,消费方不得再拿 workspace_key 拼目录)。
std::expected<WorkspaceManifest, std::string> OpenOrRegisterWorkspace(
    const std::filesystem::path& workspaces_root, const WorkspaceIdentity& identity,
    std::int64_t now_ms, bool* created_out = nullptr,
    std::filesystem::path* workspace_dir_out = nullptr);

// doctor 对账:manifest 的 identity_kind/identity_root 重算 key,与
// workspace_key 逐字比。不合回 identity.key_mismatch;kind/root 缺失回
// schema.missing_field。explicit_marker 的 seed 是声明 id(manifest 按冻结
// 合同不存它),对账须从 marker 现场读来经 marker_workspace_id 递进。
struct ManifestReconcile {
    bool ok = false;
    std::string error_code;
    std::string error_text;
    std::string expected_key;
};
ManifestReconcile ReconcileWorkspaceManifest(
    const WorkspaceManifest& manifest,
    std::optional<std::string> marker_workspace_id = std::nullopt);

}  // namespace lubancode::workspace
