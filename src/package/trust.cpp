// Package 信任的实现(账本骨架照 config/plugin_trust.cpp 的路,字段换
// 成 Package 的;审批材料与账务动作照 runtime/plugin_tool.cpp 的五样回执
// 先例)。
#include "package/trust.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

#include <nlohmann/json.hpp>

#include "hooks/hash.hpp"  // DefinitionHashShort(展示用 12 位短码)
#include "platform/paths.hpp"

namespace lubancode::package {

namespace {

using platform::PathToUtf8;

std::string JoinArgs(const std::vector<std::string>& args) {
    std::string out;
    for (const std::string& arg : args) {
        if (!out.empty()) out += ' ';
        out += arg;
    }
    return out;
}

}  // namespace

bool ScopeRequiresTrust(PackageScope scope) {
    // user = 用户亲手放进 ~/.lubancode/packages;official = 随发行走的官方
    // 包。两层的待遇与用户插件相同:视作已安装来源,不审(§9.2)。
    // project(仓库里带的)与 dev(--package-dir 挂的源码目录)是外来代
    // 码,过门。
    return scope == PackageScope::Project || scope == PackageScope::Dev;
}

bool PackageTrustSnapshot::IsTrusted(const std::string& package_id,
                                     const std::string& content_hash) const {
    return keys.count(package_id + "\n" + content_hash) > 0;
}

// ---------------------------------------------------------------------------
// PackageTrustStore
// ---------------------------------------------------------------------------

std::optional<std::string> PackageTrustStore::DefaultStorePath() {
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
    std::filesystem::path file = dir / "package-trust.json";
    return std::string(reinterpret_cast<const char*>(file.u8string().data()), file.u8string().size());
}

std::pair<PackageTrustStore, std::optional<std::string>> PackageTrustStore::Load(
    const std::optional<std::string>& path) {
    PackageTrustStore store;
    store.path_ = path;
    if (!path.has_value() || path->empty()) {
        return {std::move(store), std::nullopt};  // 纯内存模式
    }

    std::ifstream file(std::filesystem::path(reinterpret_cast<const char8_t*>(path->c_str())),
                       std::ios::binary);
    if (!file.is_open()) {
        return {std::move(store), std::nullopt};  // 首访,账本空白
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    try {
        const nlohmann::json root = nlohmann::json::parse(content);
        if (!root.is_object()) return {std::move(store), std::nullopt};
        // v1 账本:{"schema_version":1, "trusted": {"<id>\n<hash>":
        // {"version","scope","trusted_at"}}}。键里带换行,人手改 JSON 凑
        // 键很费劲——与 plugin-trust.json 的"完整目录路径 + 完整 sha256"
        // 同款脾气。
        if (root.contains("trusted") && root["trusted"].is_object()) {
            for (auto it = root["trusted"].begin(); it != root["trusted"].end(); ++it) {
                const auto& value = it.value();
                if (!value.is_object()) continue;
                PackageTrustEntry entry;
                const std::size_t split = it.key().find('\n');
                if (split == std::string::npos) continue;  // 键不整,这条不认
                entry.package_id = it.key().substr(0, split);
                entry.content_hash = it.key().substr(split + 1);
                if (entry.package_id.empty() || entry.content_hash.empty()) continue;
                if (value.contains("version") && value["version"].is_string()) {
                    entry.version = value["version"].get<std::string>();
                }
                if (value.contains("scope") && value["scope"].is_string()) {
                    entry.scope = value["scope"].get<std::string>();
                }
                if (value.contains("trusted_at") && value["trusted_at"].is_string()) {
                    entry.trusted_at_unix = value["trusted_at"].get<std::string>();
                }
                store.trusted_[it.key()] = std::move(entry);
            }
        }
    } catch (const std::exception& e) {
        // 坏账本:不崩,警告交出去,账本从空白重开(重新审一遍比带着一本
        // 读不动的账继续跑更安全)。
        store.trusted_.clear();
        return {std::move(store), std::string("Package 信任账本读不动,已按空白处理: ") + e.what()};
    }
    return {std::move(store), std::nullopt};
}

bool PackageTrustStore::IsTrusted(const std::string& package_id,
                                  const std::string& content_hash) const {
    return trusted_.count(Key(package_id, content_hash)) > 0;
}

std::optional<PackageTrustEntry> PackageTrustStore::Latest(const std::string& package_id) const {
    // map 按 "id\nhash" 排,同 id 的条目连成一片;取 trusted_at 最大的一条
    //(unix 秒的十进制串,字典序即时间序;同秒并批时让 map 序靠后的占住,
    // 结果确定)。
    std::optional<PackageTrustEntry> best;
    for (const auto& [key, entry] : trusted_) {
        if (entry.package_id != package_id) continue;
        if (!best.has_value() || entry.trusted_at_unix >= best->trusted_at_unix) {
            best = entry;
        }
    }
    return best;
}

void PackageTrustStore::SetTrusted(const std::string& package_id, const std::string& version,
                                   const std::string& content_hash, const std::string& scope) {
    PackageTrustEntry entry;
    entry.package_id = package_id;
    entry.version = version;
    entry.content_hash = content_hash;
    entry.scope = scope;
    entry.trusted_at_unix = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    trusted_[Key(package_id, content_hash)] = std::move(entry);
    dirty_ = true;
    Save();
}

std::vector<PackageTrustEntry> PackageTrustStore::Untrust(const std::string& package_id) {
    std::vector<PackageTrustEntry> removed;
    for (auto it = trusted_.begin(); it != trusted_.end();) {
        if (it->second.package_id == package_id) {
            removed.push_back(std::move(it->second));
            it = trusted_.erase(it);
        } else {
            ++it;
        }
    }
    if (!removed.empty()) {
        dirty_ = true;
        Save();
    }
    return removed;
}

std::optional<std::string> PackageTrustStore::Save() {
    if (!path_.has_value() || path_->empty() || !dirty_) {
        return std::nullopt;
    }
    nlohmann::json root;
    root["schema_version"] = 1;
    nlohmann::json trusted = nlohmann::json::object();
    for (const auto& [key, entry] : trusted_) {
        nlohmann::json value = nlohmann::json::object();
        value["version"] = entry.version;
        value["scope"] = entry.scope;
        value["trusted_at"] = entry.trusted_at_unix;
        trusted[key] = std::move(value);
    }
    root["trusted"] = std::move(trusted);

    // 原子写:先落临时文件再换名(平台层 ReplaceFileAtomically)。
    const std::filesystem::path target(reinterpret_cast<const char8_t*>(path_->c_str()));
    std::filesystem::path temp = target;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return "Package 信任账本写不进去: " + *path_;
        }
        out << root.dump(2);
        if (!out.good()) {
            return "Package 信任账本写一半失败: " + *path_;
        }
    }
    const auto replaced = platform::ReplaceFileAtomically(temp, target);
    if (!replaced.has_value()) {
        return "Package 信任账本落盘失败: " + replaced.error();
    }
    dirty_ = false;
    return std::nullopt;
}

PackageTrustSnapshot PackageTrustStore::Snapshot() const {
    PackageTrustSnapshot snapshot;
    for (const auto& [key, entry] : trusted_) {
        (void)entry;
        snapshot.keys.insert(key);
    }
    return snapshot;
}

// ---------------------------------------------------------------------------
// 审批材料与账务
// ---------------------------------------------------------------------------

std::vector<std::string> BuildPackageApprovalLines(const PackageRecord& record) {
    std::vector<std::string> lines;
    const PackageInventory& inventory = record.inventory;
    // 第一样:身份。第二样:来源。
    lines.push_back("包 " + inventory.package_id +
                    (inventory.version_text.empty() ? "" : " " + inventory.version_text) + "(" +
                    ScopeToString(inventory.scope) + " 层)");
    lines.push_back("包根: " + PathToUtf8(inventory.package_root));
    // 第三样:code 组件逐件——插件命令与逐件工具名(wire 名 + 展示名,契约
    // §九"将新增的工具名")、MCP 的 command/args/env 形状与网络声明。env
    // 只认 ${env:NAME} 占位,真值运行时才取,这里没有密钥可打码,如实说。
    std::size_t code_count = 0;
    for (const auto& component : record.components) {
        if (component.kind == ComponentKind::Plugin) {
            ++code_count;
            if (component.plugin.has_value()) {
                lines.push_back("  插件 " + component.canonical_id + "(" +
                                std::string(runtime::RuntimeKindName(component.plugin->kind)) + "/" +
                                (component.plugin->language.empty() ? std::string("-")
                                                                    : component.plugin->language) +
                                ")");
            } else {
                lines.push_back("  插件 " + component.canonical_id);
            }
            if (component.plugin.has_value()) {
                const runtime::PluginManifest& manifest = *component.plugin;
                lines.push_back("    命令: " + JoinArgs(manifest.argv) + "(不经 shell,argv 直递)");
                lines.push_back(std::string("    网络: ") +
                                (manifest.network_allowed ? "声明出网" : "不出网") + ";env 递给: " +
                                (manifest.env_allowlist.empty()
                                     ? std::string("无")
                                     : JoinArgs(manifest.env_allowlist)));
                lines.push_back("    工具 " + std::to_string(manifest.tools.size()) + " 件:");
                if (record.mount_plan.has_value()) {
                    for (const auto& entry : record.mount_plan->entries) {
                        if (entry.canonical_id != component.canonical_id) continue;
                        for (const auto& tool : entry.tools) {
                            lines.push_back("      " + tool.wire_name + "(展示 " + tool.display_name +
                                            ")");
                        }
                    }
                }
            }
        } else if (component.kind == ComponentKind::McpServer) {
            ++code_count;
            lines.push_back("  MCP " + component.canonical_id);
            if (component.mcp.has_value()) {
                const McpComponentDefinition& mcp = *component.mcp;
                std::string command = mcp.command;
                if (!mcp.args.empty()) command += " " + JoinArgs(mcp.args);
                lines.push_back("    命令: " + command + "(stdio,不经 shell)");
                if (mcp.env.empty()) {
                    lines.push_back("    env: 无");
                } else {
                    std::string env_list;
                    for (const auto& [name, value] : mcp.env) {
                        if (!env_list.empty()) env_list += ", ";
                        env_list += name + "=" + value;
                    }
                    lines.push_back("    env " + std::to_string(mcp.env.size()) + " 项:" + env_list +
                                    "(值只认 ${env:NAME} 占位,真值运行时取,不落账)");
                }
                lines.push_back(std::string("    网络: ") + (mcp.network_allowed ? "声明可出网" : "不出网") +
                                ";超时 " + std::to_string(mcp.timeout_ms) + "ms");
                lines.push_back("    工具: 握手后才知道");
            }
        }
    }
    if (code_count == 0) {
        lines.push_back("  (没有 code 组件)");
    }
    // 第四样:文件数 + 完整指纹。批的是看得见的东西——指纹全量打出,不拿
    // 短码让人抄。
    lines.push_back("文件 " + std::to_string(inventory.total_file_count) + " 个,完整内容指纹:");
    lines.push_back("  " + inventory.content_hash);
    return lines;
}

PackageTrustActionResult TrustPackage(const PackageRecord& record, PackageTrustStore* store) {
    PackageTrustActionResult result;
    const PackageInventory& inventory = record.inventory;
    if (store == nullptr) {
        result.error = "信任账不可用(找不到用户主目录),记不了账。";
        return result;
    }
    if (!inventory.manifest_ok) {
        result.error = "包 " + inventory.package_id + " 的根清单读不出 id:先 /package doctor " +
                       inventory.package_id + " 修好——审批材料出不来就批不了。";
        return result;
    }
    if (!record.valid) {
        result.error = "包 " + inventory.package_id + " 还没到能批的样子:先 /package doctor " +
                       inventory.package_id + " 把错修了(材料不全,批不了)。";
        return result;
    }
    result.ok = true;
    result.lines = BuildPackageApprovalLines(record);
    if (!inventory.code_bearing()) {
        result.lines.push_back("该包是 content-only(无 code 组件),不经信任门——内容组件照挂,"
                               "无须批准,也不记账。");
        return result;
    }
    if (!ScopeRequiresTrust(inventory.scope)) {
        result.lines.push_back("该包来自 " + ScopeToString(inventory.scope) +
                               " 层:用户亲手放/官方发布的,视作已安装来源,不进信任账"
                               "(上面材料仅供过目;要拦它用启停,不是信任)。");
        return result;
    }
    if (store->IsTrusted(inventory.package_id, inventory.content_hash)) {
        result.lines.push_back("这枚哈希已在信任账上,不用重批;会话钉快照,重启后按账放行。");
        return result;
    }
    store->SetTrusted(inventory.package_id, inventory.version_text, inventory.content_hash,
                      ScopeToString(inventory.scope));
    if (const auto saved = store->Save(); saved.has_value()) {
        result.lines.push_back("注意: " + *saved + "(账在内存里,这次会话外不保)。");
    }
    // 第五样:结论。重启生效照阶段 3 的会话钉快照语义;哈希一变即失效。
    result.lines.push_back("已信任,重启后生效(会话钉快照,运行中的会话不换账)。批的是上面这枚"
                           "哈希:包内文件改一个字节它就变,信任失效,须重批"
                           "(/package trust " + inventory.package_id + ")。");
    return result;
}

PackageTrustActionResult UntrustPackage(const PackageRecord& record, PackageTrustStore* store) {
    PackageTrustActionResult result;
    const PackageInventory& inventory = record.inventory;
    if (store == nullptr) {
        result.error = "信任账不可用(找不到用户主目录),记不了账。";
        return result;
    }
    if (!inventory.manifest_ok) {
        result.error = "包 " + inventory.package_id + " 的根清单读不出 id:先 /package doctor " +
                       inventory.package_id + " 修好再销账。";
        return result;
    }
    result.ok = true;
    if (!inventory.code_bearing()) {
        result.lines.push_back("包 " + inventory.package_id +
                               " 是 content-only(无 code 组件),从不进信任账,没有可销的。");
        return result;
    }
    if (!ScopeRequiresTrust(inventory.scope)) {
        result.lines.push_back("该包来自 " + ScopeToString(inventory.scope) +
                               " 层,视作已安装来源,不进信任账——没有可销的。");
        return result;
    }
    const std::vector<PackageTrustEntry> removed = store->Untrust(inventory.package_id);
    if (removed.empty()) {
        result.lines.push_back("包 " + inventory.package_id +
                               " 名下没有信任账(可能从没批过,或批后已销)。");
        return result;
    }
    result.lines.push_back("包 " + inventory.package_id + " 已销信任 " +
                           std::to_string(removed.size()) + " 条:");
    for (const auto& entry : removed) {
        result.lines.push_back("  版本 " + (entry.version.empty() ? "-" : entry.version) + ",指纹 " +
                               entry.content_hash);
    }
    result.lines.push_back("重启后 code 组件不再具备挂载资格;内容组件照挂(阶段 3 语义)。要再挂,"
                           "/package trust " + inventory.package_id + " 重批即可。");
    return result;
}

std::string DescribeTrustStatus(const PackageInventory& inventory, const PackageTrustStore* store) {
    if (!inventory.code_bearing()) {
        return "无 code 组件,不经信任门";
    }
    if (!ScopeRequiresTrust(inventory.scope)) {
        return std::string(ScopeToString(inventory.scope)) + " 层视作已安装来源,不经信任门";
    }
    if (store != nullptr && store->IsTrusted(inventory.package_id, inventory.content_hash)) {
        return "已信任(这枚哈希在账上,重启后放行 code 组件)";
    }
    if (store != nullptr) {
        if (const auto latest = store->Latest(inventory.package_id); latest.has_value()) {
            return "信任已失效:文件动过(批的是 " +
                   hooks::DefinitionHashShort(latest->content_hash) + "…,现在是 " +
                   hooks::DefinitionHashShort(inventory.content_hash) +
                   "…)。重新批准: /package trust " + inventory.package_id;
        }
    }
    return "未信任:code 组件一件不挂不执行。批准: /package trust " + inventory.package_id;
}

}  // namespace lubancode::package
