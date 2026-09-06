// workspace v2 manifest 读写与对账的实现(P0-1)。
#include "workspace/manifest.hpp"

#include <fstream>
#include <sstream>

#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1)
#include "platform/paths.hpp"
#include "workspace/index.hpp"  // 账本制:查账/记账/门牌
#include "workspace/storage_contracts.hpp"

namespace lubancode::workspace {
namespace {

namespace fs = std::filesystem;
using platform::PathToUtf8;

bool WriteTextFileAtomic(const fs::path& path, const std::string& content) {
    return platform::AtomicWriteFile(path, content).has_value();
}

std::string ReadTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

}  // namespace

nlohmann::json WorkspaceManifest::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema"] = std::string(contracts::kWorkspaceSchemaName);
    json["version"] = contracts::kWorkspaceSchemaVersion;
    json["workspace_key"] = workspace_key;
    json["display_name"] = display_name;
    json["identity_kind"] = identity_kind;
    json["identity_root"] = identity_root;
    json["created_at_ms"] = created_at_ms;
    json["last_opened_at_ms"] = last_opened_at_ms;
    nlohmann::json checkouts_json = nlohmann::json::array();
    for (const WorkspaceCheckout& checkout : checkouts) {
        checkouts_json.push_back(nlohmann::json{
            {"root", checkout.root},
            {"first_seen_at_ms", checkout.first_seen_at_ms},
            {"last_seen_at_ms", checkout.last_seen_at_ms},
        });
    }
    json["checkouts"] = std::move(checkouts_json);
    if (migrated_from.has_value()) {
        json["migrated_from"] = *migrated_from;
    }
    return json;
}

std::optional<WorkspaceManifest> WorkspaceManifest::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    WorkspaceManifest manifest;
    const auto read_string = [&](const char* key, std::string* out) {
        const auto it = json.find(key);
        if (it == json.end() || !it->is_string()) return false;
        *out = it->get<std::string>();
        return true;
    };
    if (!read_string("workspace_key", &manifest.workspace_key) ||
        !read_string("identity_kind", &manifest.identity_kind)) {
        return std::nullopt;
    }
    read_string("display_name", &manifest.display_name);
    read_string("identity_root", &manifest.identity_root);
    const auto read_ms = [&](const char* key, std::int64_t* out) {
        const auto it = json.find(key);
        if (it == json.end() || !it->is_number_integer()) return;
        *out = it->get<std::int64_t>();
    };
    read_ms("created_at_ms", &manifest.created_at_ms);
    read_ms("last_opened_at_ms", &manifest.last_opened_at_ms);
    const auto checkouts = json.find("checkouts");
    if (checkouts != json.end() && checkouts->is_array()) {
        for (const auto& entry : *checkouts) {
            if (!entry.is_object() || !entry.contains("root") || !entry.at("root").is_string()) {
                continue;
            }
            WorkspaceCheckout checkout;
            checkout.root = entry.at("root").get<std::string>();
            if (entry.contains("first_seen_at_ms") && entry.at("first_seen_at_ms").is_number_integer()) {
                checkout.first_seen_at_ms = entry.at("first_seen_at_ms").get<std::int64_t>();
            }
            if (entry.contains("last_seen_at_ms") && entry.at("last_seen_at_ms").is_number_integer()) {
                checkout.last_seen_at_ms = entry.at("last_seen_at_ms").get<std::int64_t>();
            }
            manifest.checkouts.push_back(std::move(checkout));
        }
    }
    const auto migrated = json.find("migrated_from");
    if (migrated != json.end() && migrated->is_object()) {
        manifest.migrated_from = *migrated;
    }
    return manifest;
}

ManifestRead ReadWorkspaceManifest(const fs::path& workspace_dir) {
    ManifestRead read;
    const fs::path path = workspace_dir / "workspace.json";
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        read.status = ManifestRead::Status::Missing;
        return read;
    }
    const auto json = nlohmann::json::parse(ReadTextFile(path), nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        read.status = ManifestRead::Status::Corrupt;
        read.error_code = "schema.missing_field";
        read.error_text = "workspace.json 解不开: " + PathToUtf8(path);
        return read;
    }
    // 版本协商(合同 §七):version 缺失按缺省版兜;> reader 上限整份拒读,
    // 不猜、不静默降级、不部分解析。
    int version = contracts::kWorkspaceSchemaVersion;
    const auto version_it = json.find("version");
    if (version_it != json.end() && version_it->is_number_integer()) {
        version = version_it->get<int>();
    } else {
        read.error_code = "schema.missing_field";  // 诊断级注记,仍按缺省版读
    }
    if (version > contracts::kWorkspaceSchemaVersion) {
        read.status = ManifestRead::Status::UnsupportedVersion;
        read.error_code = "schema.unsupported_version";
        read.error_text = "workspace.json version=" + std::to_string(version) +
                          " 超出本版 reader 上限";
        return read;
    }
    auto manifest = WorkspaceManifest::FromJson(json);
    if (!manifest.has_value()) {
        read.status = ManifestRead::Status::Corrupt;
        if (read.error_code.empty()) read.error_code = "schema.missing_field";
        read.error_text = "workspace.json 缺必填键: " + PathToUtf8(path);
        return read;
    }
    read.status = ManifestRead::Status::Ok;
    read.manifest = std::move(*manifest);
    return read;
}

std::expected<void, std::string> WriteWorkspaceManifestAtomic(const fs::path& workspace_dir,
                                                              const WorkspaceManifest& manifest) {
    const fs::path path = workspace_dir / "workspace.json";
    if (!WriteTextFileAtomic(path, manifest.ToJson().dump())) {
        return std::unexpected("workspace.open_failed: workspace.json 原子写失败: " +
                               PathToUtf8(path));
    }
    return {};
}

std::expected<WorkspaceManifest, std::string> OpenOrRegisterWorkspace(
    const fs::path& workspaces_root, const WorkspaceIdentity& identity, std::int64_t now_ms,
    bool* created_out, fs::path* workspace_dir_out) {
    if (!identity.valid()) {
        return std::unexpected("identity.path_invalid: 身份没裁决出 workspace_key");
    }
    // 账本制找门三步(账本制单子 §一):查账→miss 生门牌→开房记账。
    // 门牌 ≠ workspace_key:目录名是装饰,身份仍在 manifest/session.json;
    // 消费方一律经 workspace_dir_out/账本取房门,不得拿 key 拼目录。
    const std::string index_key = index::CanonicalIndexKey(identity);
    std::string dir_name;
    if (const auto hit = index::LookupWorkspaceDir(workspaces_root, index_key)) {
        std::error_code hit_ec;
        if (fs::is_directory(workspaces_root / platform::Utf8ToPath(*hit), hit_ec) && !hit_ec) {
            dir_name = *hit;  // 账上有门,房也在盘上
        }
    }
    if (dir_name.empty()) {
        // miss(新项目/账本丢账/房被手删):门牌是纯函数,同 identity 恒同
        // 名——重算即回原房,不裂房;真新项目才开新房。
        dir_name = index::MakeWorkspaceDirName(identity);
    }
    const fs::path workspace_dir = workspaces_root / platform::Utf8ToPath(dir_name);
    std::error_code ec;
    fs::create_directories(workspace_dir, ec);
    if (ec) {
        return std::unexpected("workspace.open_failed: workspace 目录建不起: " +
                               PathToUtf8(workspace_dir) + ": " + ec.message());
    }
    if (created_out != nullptr) {
        *created_out = false;
    }

    const ManifestRead read = ReadWorkspaceManifest(workspace_dir);
    if (read.status == ManifestRead::Status::UnsupportedVersion ||
        read.status == ManifestRead::Status::Corrupt) {
        return std::unexpected(read.error_code + ": " + read.error_text);
    }
    WorkspaceManifest manifest;
    if (read.status == ManifestRead::Status::Missing) {
        // 首仓:checkouts 只记当前检出,身份四件从裁决结果来。
        manifest.workspace_key = identity.workspace_key;
        manifest.display_name = identity.display_name;
        manifest.identity_kind = identity.identity_kind;
        manifest.identity_root = NormalizeIdentityPathText(identity.identity_root);
        manifest.created_at_ms = now_ms;
        manifest.last_opened_at_ms = now_ms;
        WorkspaceCheckout checkout;
        checkout.root = NormalizeIdentityPathText(identity.checkout_root);
        checkout.first_seen_at_ms = now_ms;
        checkout.last_seen_at_ms = now_ms;
        manifest.checkouts.push_back(std::move(checkout));
        if (created_out != nullptr) {
            *created_out = true;
        }
    } else {
        manifest = std::move(read.manifest);
        if (manifest.workspace_key != identity.workspace_key) {
            return std::unexpected(std::string(contracts::kErrIdentityKeyMismatch) +
                                   ": manifest key=" + manifest.workspace_key +
                                   " 与算法重算 key=" + identity.workspace_key +
                                   " 不合,已隔离;不自动改名合并,请跑 doctor 对账");
        }
        manifest.last_opened_at_ms = now_ms;
        // checkout upsert:按规范化 root 匹配;同 root 只更新 last_seen。
        const std::string root_text = NormalizeIdentityPathText(identity.checkout_root);
        bool found = false;
        for (WorkspaceCheckout& checkout : manifest.checkouts) {
            if (NormalizeIdentityPathText(platform::Utf8ToPath(checkout.root)) == root_text) {
                checkout.last_seen_at_ms = now_ms;
                found = true;
                break;
            }
        }
        if (!found) {
            WorkspaceCheckout checkout;
            checkout.root = root_text;
            checkout.first_seen_at_ms = now_ms;
            checkout.last_seen_at_ms = now_ms;
            manifest.checkouts.push_back(std::move(checkout));
        }
    }
    if (const auto written = WriteWorkspaceManifestAtomic(workspace_dir, manifest);
        !written.has_value()) {
        return std::unexpected(written.error());
    }
    // 记账:房已开门、manifest 落盘,账本并这一笔(原子写)。失败不拦
    // 开张——账本是可重建缓存,房自描述在盘上,丢了靠重建/下次开张自愈。
    index::RecordWorkspaceEntry(workspaces_root, index_key, dir_name, manifest.created_at_ms);
    if (workspace_dir_out != nullptr) {
        *workspace_dir_out = workspace_dir;
    }
    return manifest;
}

ManifestReconcile ReconcileWorkspaceManifest(const WorkspaceManifest& manifest,
                                             std::optional<std::string> marker_workspace_id) {
    ManifestReconcile result;
    std::string seed;
    const std::string_view kind = manifest.identity_kind;
    if (kind == contracts::kIdentityKindGitCommon) {
        seed = std::string(contracts::kSeedPrefixGit) + manifest.identity_root;
    } else if (kind == contracts::kIdentityKindExplicitMarker) {
        // marker 的 seed 是声明 id,不是路径;manifest 按冻结合同不存
        // workspace_id(P0-0 合同 §二),对账须带 marker 现场的 id 来。
        if (!marker_workspace_id.has_value()) {
            result.error_code = "schema.missing_field";
            result.error_text =
                "explicit_marker 的 manifest 不携 workspace_id,重算须带 marker 现场声明 id";
            return result;
        }
        seed = std::string(contracts::kSeedPrefixMarker) + *marker_workspace_id;
    } else if (kind == contracts::kIdentityKindConfigRoot || kind == contracts::kIdentityKindCwdFallback) {
        seed = std::string(contracts::kSeedPrefixPath) + manifest.identity_root;
    } else {
        result.error_code = "schema.missing_field";
        result.error_text = "identity_kind 不在四值封闭集: " + manifest.identity_kind;
        return result;
    }
    if (manifest.identity_root.empty()) {
        result.error_code = "schema.missing_field";
        result.error_text = "identity_root 缺失,无法重算 key";
        return result;
    }
    result.expected_key = ComputeWorkspaceKeyFromSeed(seed, manifest.display_name);
    if (result.expected_key != manifest.workspace_key) {
        result.error_code = std::string(contracts::kErrIdentityKeyMismatch);
        result.error_text = "manifest key=" + manifest.workspace_key +
                            " 与算法重算 key=" + result.expected_key + " 不合";
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace lubancode::workspace
