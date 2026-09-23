// workspace v2 manifest 读写与对账的实现(P0-1)。
#include "workspace/manifest.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1)
#include "platform/paths.hpp"
#include "workspace/index.hpp"          // 账本制:查账/记账/门牌
#include "workspace/manifest_lock.hpp"  // SV-11:读改写事务锁(跨进程)
#include "workspace/storage_contracts.hpp"

namespace lubancode::workspace {
namespace {

namespace fs = std::filesystem;
using platform::PathToUtf8;

bool WriteTextFileAtomic(const fs::path& path, const std::string& content) {
    return platform::AtomicWriteFile(path, content).has_value();
}

// Windows 瞬态文件语义的有界重试档(单位:次数 / 退避步长)。三案 CI 实测
// (2026-09 run 34316108135、34335606083、34360062483,windows-msvc 腿):
// 原子换名(MoveFileExW REPLACE)与防病毒/索引过滤驱动会让目标文件的
// 元数据查询(fs::exists)与打开(ifstream 不带 FILE_SHARE_DELETE)短
// 拒,拦截窗实测可达几十毫秒(坏读全是"10×2ms 重试耗尽"型;写侧 320
// 次换名被拒 48-57 次)——不是微秒级换名窗本身。10 次×10ms=100ms 预算
// 给足余量;正常路径首次即成,零等待零重试。POSIX rename 原子、无共享
// 违例,下列瞬态恒不发生,重试路径零开销零行为变化。
constexpr int kTransientReadAttempts = 10;
constexpr int kTransientWriteAttempts = 10;
constexpr std::chrono::milliseconds kTransientReadBackoff{10};
constexpr std::chrono::milliseconds kTransientWriteBackoff{10};

// SV-11:登记事务锁的有界等待档(60×100ms=6s)。对头活持有者放手要时间
// ——锁内只做一份小 JSON 的读改写,常态毫秒级;开房是启动路径,烧完仍
// 撞就如实回 workspace.locked,不无限等、不悄悄覆盖旧账。持有者暴毙不等
// 钟:身份核判死即隔离接手。档的账:等待窗要装下"同房排队深度×单手
// 持锁时长"——单手持锁含读档/原子写/记账各自的瞬态重试档(windows 慢
// 盘单手可达数百 ms),旧档 2s 装不下三手以上排队,windows-msvc 腿 ledger
// 册并发开房段(8 手同房)三案间歇红(2026-09 run 35611620658 att1 /
// 35668147611 att1 / 35667534366 att6):队尾烧窗被顶翻。6s 对 8 手×
// 500ms 仍有余量;正常路径首次即得,零等待。
constexpr int kLockWaitAttempts = 60;
constexpr int kLockWaitIntervalMs = 100;

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
    // 探测/打开的瞬态失败有界重试(依据见 kTransientRead* 注释)。确定性
    // 答案不重试、判据不动:查无此物(ec 清)立即 Missing;读出的内容
    // parse 坏立即 Corrupt——撕裂 JSON 重读恒坏,该红就红。旧账:打不开
    // 曾折成空串喂 parse,判成 Corrupt(schema.missing_field),并发开房
    // 路零宽限直接落空,上列三案同根于此窗。
    std::string text;
    bool have_text = false;
    std::string transient_code;
    for (int attempt = 0;; ++attempt) {
        std::error_code ec;
        if (const bool present = fs::exists(path, ec); !ec) {
            if (!present) {
                read.status = ManifestRead::Status::Missing;  // 真没有:立即定案
                return read;
            }
            if (std::ifstream file(path, std::ios::binary); file.is_open()) {
                std::stringstream buffer;
                buffer << file.rdbuf();
                text = buffer.str();
                have_text = true;
                break;
            }
            transient_code = "read.open_failed";
        } else {
            transient_code = "read.probe_failed";
        }
        if (attempt >= kTransientReadAttempts) {
            break;
        }
        std::this_thread::sleep_for(kTransientReadBackoff);
    }
    if (!have_text) {
        // 重试耗尽:status 判据沿旧账(探测败按缺、打开败按坏),error_code
        // 把"读不成"与"内容坏"分开——诊断不再冒充 schema 病。
        if (transient_code == "read.open_failed") {
            read.status = ManifestRead::Status::Corrupt;
        } else {
            read.status = ManifestRead::Status::Missing;
        }
        read.error_code = transient_code;
        read.error_text = std::string(transient_code == "read.open_failed"
                                          ? "workspace.json 打不开(重试耗尽): "
                                          : "workspace.json 探测被拒(重试耗尽): ") +
                          PathToUtf8(path);
        return read;
    }
    const auto json = nlohmann::json::parse(text, nullptr, false);
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

    // SV-11:workspace.json 的读→校验→checkout upsert→写是跨进程临界区。
    // 两个 linked worktree 同房并发开张,A 读 [main]、B 读 [main]、A 写
    // [main,A]、B 写 [main,B]——四次操作都成功,A 的登记凭空消失;原子
    // 替换只防半份文件,不防读改写交错。事务锁把整段串行:锁后重读,时间
    // 戳取单调最大值。锁粒度=这一间房(不同 workspace 互不阻塞);持有者
    // 暴毙走陈锁隔离留证;超时有界,如实回 workspace.locked。
    ManifestLock manifest_lock;
    const auto lock_result = ManifestLock::Acquire(workspace_dir, &manifest_lock, kLockWaitAttempts,
                                                   kLockWaitIntervalMs);
    if (lock_result.status != ManifestLock::Status::Acquired) {
        // 活持有/在建窗口/owner 读不懂:一律拒,不悄悄覆盖旧账;真 IO 失败
        // 照 open_failed 报,不冒充争用。
        const std::string code = lock_result.status == ManifestLock::Status::IoError
                                     ? std::string(contracts::kErrWorkspaceOpenFailed)
                                     : std::string(contracts::kErrWorkspaceLocked);
        return std::unexpected(code + ": workspace.json 登记事务锁取不上: " +
                               lock_result.detail);
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
        // 时间戳单调:对手的钟可能比盘上账慢(跨进程钟差/测试注入),旧值
        // 不许被后写改回去(SV-11 验收:last_seen/last_opened 不倒退)。
        manifest.last_opened_at_ms = std::max(manifest.last_opened_at_ms, now_ms);
        // checkout upsert:按规范化 root 匹配;同 root 只更新 last_seen
        //(单调最大),first_seen 永不改写。
        const std::string root_text = NormalizeIdentityPathText(identity.checkout_root);
        bool found = false;
        for (WorkspaceCheckout& checkout : manifest.checkouts) {
            if (NormalizeIdentityPathText(platform::Utf8ToPath(checkout.root)) == root_text) {
                checkout.last_seen_at_ms = std::max(checkout.last_seen_at_ms, now_ms);
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
    // manifest 落盘:Windows 上原子换名会被并发读者的句柄短拒(MoveFileExW
    // 对无 FILE_SHARE_DELETE 的打开方报错,identity 册 CI 实测 320 次换名
    // 拒 48-57 次)。未提交的失败(target 原样)、整份重写幂等——有界重试,
    // 耗尽才如实落空(POSIX rename 原子,首次即成,重试路径零开销)。
    // 提交阶段沿 FD-04 合同分账:换名已生效的失败只有"新内容可见、父目录
    // 刷盘未确认"(CommittedDurabilityUnconfirmed)一种——不当没写过盲目
    // 重写,也不当失败回滚;锁内下一只手会重读到新内容。AtomicVisibility
    // 档不请求目录刷盘,这格是合同防御位,当前不产生。
    const fs::path manifest_path = workspace_dir / "workspace.json";
    for (int attempt = 0;; ++attempt) {
        const auto written = platform::AtomicWriteFile(manifest_path, manifest.ToJson().dump());
        if (written.has_value()) {
            break;  // CommittedDurabilityNotRequested:换名已生效
        }
        const platform::AtomicWriteError& write_error = written.error();
        if (write_error.outcome != platform::WriteOutcome::NotCommitted) {
            break;  // 换名已生效:不当没写过(FD-04)
        }
        if (attempt >= kTransientWriteAttempts) {
            return std::unexpected(
                std::string("workspace.open_failed: workspace.json 原子写失败: ") +
                write_error.code + ": " + write_error.message + ": " + PathToUtf8(manifest_path));
        }
        std::this_thread::sleep_for(kTransientWriteBackoff);
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
