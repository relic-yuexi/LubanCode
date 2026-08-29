// Package 启停账的实现(账本骨架照 package/trust.cpp 的路:原子写、坏账
// 容错读、幂等记账;启停的缺省态与信任相反——没有账 = 启用)。
#include "package/state.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"

namespace lubancode::package {

namespace {

std::string NowUnixText() {
    return std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

}  // namespace

// ---------------------------------------------------------------------------
// PackageStateStore
// ---------------------------------------------------------------------------

std::optional<std::string> PackageStateStore::DefaultStatePath() {
    const auto home = platform::HomeDir();
    if (!home.has_value() || home->empty()) {
        return std::nullopt;
    }
    std::filesystem::path dir(*home);
    dir /= ".lubancode";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);  // 已存在不算错
    if (ec) {
        return std::nullopt;
    }
    std::filesystem::path file = dir / "package-state.json";
    return std::string(reinterpret_cast<const char*>(file.u8string().data()), file.u8string().size());
}

std::pair<PackageStateStore, std::optional<std::string>> PackageStateStore::Load(
    const std::optional<std::string>& path) {
    PackageStateStore store;
    store.path_ = path;
    if (!path.has_value() || path->empty()) {
        return {std::move(store), std::nullopt};  // 纯内存模式
    }

    std::ifstream file(std::filesystem::path(reinterpret_cast<const char8_t*>(path->c_str())),
                       std::ios::binary);
    if (!file.is_open()) {
        return {std::move(store), std::nullopt};  // 首访,账面空白(全启用)
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    try {
        const nlohmann::json root = nlohmann::json::parse(content);
        if (!root.is_object()) return {std::move(store), std::nullopt};
        // v1 账本:{"schema_version":1, "packages": {"<package-id>":
        // {"enabled","version","scope","changed_at"}}}。只记改过启停的包;
        // 没在账上的包一概启用。
        if (root.contains("packages") && root["packages"].is_object()) {
            for (auto it = root["packages"].begin(); it != root["packages"].end(); ++it) {
                const auto& value = it.value();
                if (!value.is_object()) continue;
                PackageStateEntry entry;
                entry.package_id = it.key();
                if (entry.package_id.empty()) continue;
                if (value.contains("enabled") && value["enabled"].is_boolean()) {
                    entry.enabled = value["enabled"].get<bool>();
                } else {
                    continue;  // 没有 enabled 这一笔的记录不整,这条不认
                }
                if (value.contains("version") && value["version"].is_string()) {
                    entry.version = value["version"].get<std::string>();
                }
                if (value.contains("scope") && value["scope"].is_string()) {
                    entry.scope = value["scope"].get<std::string>();
                }
                if (value.contains("changed_at") && value["changed_at"].is_string()) {
                    entry.changed_at_unix = value["changed_at"].get<std::string>();
                }
                store.states_[it.key()] = std::move(entry);
            }
        }
    } catch (const std::exception& e) {
        // 坏账本:不崩,警告交出去,账面从空白重开(视作全启用——启停是
        // "别挂谁"的账,读不动时宁可照缺省跑,把警告亮给人)。
        store.states_.clear();
        return {std::move(store), std::string("Package 启停账读不动,已按全启用处理: ") + e.what()};
    }
    return {std::move(store), std::nullopt};
}

bool PackageStateStore::IsEnabled(const std::string& package_id) const {
    const auto it = states_.find(package_id);
    return it == states_.end() || it->second.enabled;
}

std::optional<PackageStateEntry> PackageStateStore::Find(const std::string& package_id) const {
    const auto it = states_.find(package_id);
    if (it == states_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool PackageStateStore::SetEnabled(const std::string& package_id, const std::string& version,
                                   const std::string& scope, bool enabled) {
    if (IsEnabled(package_id) == enabled) {
        return false;  // 同态:幂等,不重写(改动的时刻不漂)
    }
    PackageStateEntry entry;
    entry.package_id = package_id;
    entry.enabled = enabled;
    entry.version = version;
    entry.scope = scope;
    entry.changed_at_unix = NowUnixText();
    states_[package_id] = std::move(entry);
    dirty_ = true;
    Save();
    return true;
}

PackageStateSnapshot PackageStateStore::Snapshot() const {
    PackageStateSnapshot snapshot;
    for (const auto& [id, entry] : states_) {
        if (!entry.enabled) {
            snapshot.disabled.insert(id);
        }
    }
    return snapshot;
}

std::optional<std::string> PackageStateStore::Save() {
    if (!path_.has_value() || path_->empty() || !dirty_) {
        return std::nullopt;
    }
    nlohmann::json root;
    root["schema_version"] = 1;
    nlohmann::json packages = nlohmann::json::object();
    for (const auto& [id, entry] : states_) {
        nlohmann::json value = nlohmann::json::object();
        value["enabled"] = entry.enabled;
        value["version"] = entry.version;
        value["scope"] = entry.scope;
        value["changed_at"] = entry.changed_at_unix;
        packages[id] = std::move(value);
    }
    root["packages"] = std::move(packages);

    // 原子写:先落临时文件再换名(平台层 ReplaceFileAtomically),照信任
    // 账的同款惯例。
    const std::filesystem::path target(reinterpret_cast<const char8_t*>(path_->c_str()));
    std::filesystem::path temp = target;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return "Package 启停账写不进去: " + *path_;
        }
        out << root.dump(2);
        if (!out.good()) {
            return "Package 启停账写一半失败: " + *path_;
        }
    }
    const auto replaced = platform::ReplaceFileAtomically(temp, target);
    if (!replaced.has_value()) {
        return "Package 启停账落盘失败: " + replaced.error();
    }
    dirty_ = false;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 账务
// ---------------------------------------------------------------------------

PackageStateActionResult EnableDisablePackage(const PackageInventory& inventory,
                                              PackageStateStore* store, bool enable,
                                              int session_mounted_count) {
    PackageStateActionResult result;
    const std::string& id = inventory.package_id;
    if (store == nullptr) {
        result.error = "启停账不可用(找不到用户主目录),记不了账。";
        return result;
    }
    if (!inventory.manifest_ok) {
        result.error = "包 " + id + " 的根清单读不出身份:先 /package doctor " + id +
                       " 修好——启停按包 id 记账,身份出不来就记不了。";
        return result;
    }
    result.ok = true;
    const std::string identity =
        id + (inventory.version_text.empty() ? "" : " " + inventory.version_text) + "(" +
        ScopeToString(inventory.scope) + " 层)";
    const bool changed = store->SetEnabled(id, inventory.version_text,
                                           ScopeToString(inventory.scope), enable);
    if (enable) {
        if (!changed && store->Find(id).has_value()) {
            result.lines.push_back("包 " + identity + " 本来就启用着,账未动。");
        } else {
            result.lines.push_back("已启用 " + identity + "。");
        }
        result.lines.push_back("生效时机:下回启动(或 /package reload 重折快照)——会话钉着启动时的快照,"
                               "运行中的会话不换账。code 组件(Plugin/MCP)挂载只在会话启动跑,"
                               "还须新会话。");
        return result;
    }
    // disable:回执如实说"下回启动生效",在跑的一件不拆。
    if (!changed && store->Find(id).has_value()) {
        result.lines.push_back("包 " + identity + " 本来就停着,账未动。");
    } else {
        result.lines.push_back("已停用 " + identity + "。");
    }
    result.lines.push_back("生效时机:下回启动(或 /package reload 重折快照)——挂载一律跳过,"
                           "连内容组件一件不挂;扫描发现照旧(list/doctor 可见,list 标 disabled)。");
    if (session_mounted_count > 0) {
        result.lines.push_back("本会话钉着启动时的快照,在跑的 Agent/Workflow 不拆——它名下 " +
                               std::to_string(session_mounted_count) + " 件内容组件这场照旧;");
    } else if (session_mounted_count == 0) {
        result.lines.push_back("本会话本就没挂它(启动时不在或已停用),不拆什么。");
    }
    result.lines.push_back("重新启用: /package enable " + id);
    return result;
}

std::string DescribeStateStatus(const PackageInventory& inventory, const PackageStateStore* store) {
    if (store != nullptr && !store->IsEnabled(inventory.package_id)) {
        return "已停用(挂载跳过,连内容组件一件不挂;恢复: /package enable " +
               inventory.package_id + ")";
    }
    return "启用";
}

}  // namespace lubancode::package
