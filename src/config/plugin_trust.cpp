// 项目插件信任库的实现(照 hooks/trust.cpp 的账本骨架,字段换成本库的)。
#include "config/plugin_trust.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

#include <nlohmann/json.hpp>

#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1)
#include "platform/paths.hpp"

namespace lubancode::config {

std::optional<std::string> PluginTrustStore::DefaultStorePath() {
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
    std::filesystem::path file = dir / "plugin-trust.json";
    return std::string(reinterpret_cast<const char*>(file.u8string().data()), file.u8string().size());
}

std::pair<PluginTrustStore, std::optional<std::string>> PluginTrustStore::Load(
    const std::optional<std::string>& path) {
    PluginTrustStore store;
    store.path_ = path;
    if (!path.has_value() || path->empty()) {
        return {std::move(store), std::nullopt};  // 纯内存模式
    }

    std::ifstream file(std::filesystem::path(reinterpret_cast<const char8_t*>(path->c_str())), std::ios::binary);
    if (!file.is_open()) {
        return {std::move(store), std::nullopt};  // 首访,账本空白
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    try {
        const nlohmann::json root = nlohmann::json::parse(content);
        if (root.is_object()) {
            if (root.contains("trusted") && root["trusted"].is_object()) {
                for (auto it = root["trusted"].begin(); it != root["trusted"].end(); ++it) {
                    const auto& value = it.value();
                    if (value.is_object() && value.contains("description") && value["description"].is_string()) {
                        TrustEntry entry;
                        entry.description = value["description"].get<std::string>();
                        if (value.contains("trusted_at") && value["trusted_at"].is_string()) {
                            entry.trusted_at_unix = value["trusted_at"].get<std::string>();
                        }
                        store.trusted_[it.key()] = std::move(entry);
                    }
                }
            }
            if (root.contains("disabled") && root["disabled"].is_object()) {
                for (auto it = root["disabled"].begin(); it != root["disabled"].end(); ++it) {
                    if (it.value().is_boolean()) {
                        store.disabled_[it.key()] = it.value().get<bool>();
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        // 坏账本:不崩,警告交出去,账本从空白重开(重新审一遍比带着一本
        // 读不动的账继续跑更安全)。
        store.trusted_.clear();
        store.disabled_.clear();
        return {std::move(store), std::string("插件信任账本读不动,已按空白处理: ") + e.what()};
    }
    return {std::move(store), std::nullopt};
}

bool PluginTrustStore::IsTrusted(const std::string& plugin_path, const std::string& content_hash) const {
    return trusted_.count(Key(plugin_path, content_hash)) > 0;
}

bool PluginTrustStore::SetTrusted(const std::string& plugin_path, const std::string& content_hash,
                                  const std::string& description) {
    TrustEntry entry;
    entry.description = description;
    entry.trusted_at_unix = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    trusted_[Key(plugin_path, content_hash)] = std::move(entry);
    dirty_ = true;
    Save();
    return true;
}

void PluginTrustStore::Untrust(const std::string& plugin_path, const std::string& content_hash) {
    trusted_.erase(Key(plugin_path, content_hash));
    dirty_ = true;
    Save();
}

bool PluginTrustStore::IsDisabled(const std::string& plugin_path, const std::string& content_hash) const {
    const auto it = disabled_.find(Key(plugin_path, content_hash));
    return it != disabled_.end() && it->second;
}

void PluginTrustStore::SetDisabled(const std::string& plugin_path, const std::string& content_hash,
                                   bool disabled) {
    disabled_[Key(plugin_path, content_hash)] = disabled;
    dirty_ = true;
    Save();
}

std::optional<std::string> PluginTrustStore::Save() {
    if (!path_.has_value() || path_->empty() || !dirty_) {
        return std::nullopt;
    }
    nlohmann::json root;
    root["schema_version"] = 1;
    nlohmann::json trusted = nlohmann::json::object();
    for (const auto& [key, entry] : trusted_) {
        trusted[key]["description"] = entry.description;
        trusted[key]["trusted_at"] = entry.trusted_at_unix;
    }
    root["trusted"] = std::move(trusted);
    nlohmann::json disabled = nlohmann::json::object();
    for (const auto& [key, value] : disabled_) {
        disabled[key] = value;
    }
    root["disabled"] = std::move(disabled);

    // 原子写,统一走 platform::AtomicWriteFile(插件信任账是持久事实)。
    const std::filesystem::path target(reinterpret_cast<const char8_t*>(path_->c_str()));
    const auto written = platform::AtomicWriteFile(target, root.dump(2));
    if (!written.has_value()) {
        return "插件信任账本落盘失败: " + written.error().message;
    }
    dirty_ = false;
    return std::nullopt;
}

}  // namespace lubancode::config
