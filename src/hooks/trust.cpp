#include "hooks/trust.hpp"

#include <chrono>
#include <fstream>

#include <nlohmann/json.hpp>

#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1:只动落盘机制,不动 Hook 语义)
#include "platform/paths.hpp"

namespace lubancode::hooks {

std::optional<std::string> HookTrustStore::DefaultStorePath() {
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
    std::filesystem::path file = dir / "hook-trust.json";
    return std::string(reinterpret_cast<const char*>(file.u8string().data()), file.u8string().size());
}

std::pair<HookTrustStore, std::optional<std::string>> HookTrustStore::Load(
    const std::optional<std::string>& path) {
    HookTrustStore store;
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
                    if (value.is_object() && value.contains("command") && value["command"].is_string()) {
                        TrustEntry entry;
                        entry.command = value["command"].get<std::string>();
                        if (value.contains("trusted_at") && value["trusted_at"].is_number_integer()) {
                            entry.trusted_at = value["trusted_at"].get<std::int64_t>();
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
        // 坏账本:不崩,警告交出去,账本从空白重开(用户重新审一遍比带着
        // 一本读不动的账继续跑更安全)。
        store.trusted_.clear();
        store.disabled_.clear();
        return {std::move(store), std::string("hook 信任账本读不动,已按空白处理: ") + e.what()};
    }
    return {std::move(store), std::nullopt};
}

bool HookTrustStore::IsTrusted(const std::string& source_path, const std::string& definition_hash) const {
    return trusted_.count(Key(source_path, definition_hash)) > 0;
}

bool HookTrustStore::SetTrusted(const std::string& source_path, const std::string& definition_hash,
                                const std::string& command) {
    TrustEntry entry;
    entry.command = command;
    entry.trusted_at = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    trusted_[Key(source_path, definition_hash)] = std::move(entry);
    dirty_ = true;
    Save();
    return true;
}

void HookTrustStore::Untrust(const std::string& source_path, const std::string& definition_hash) {
    trusted_.erase(Key(source_path, definition_hash));
    dirty_ = true;
    Save();
}

bool HookTrustStore::IsDisabled(const std::string& source_path, const std::string& definition_hash) const {
    const auto it = disabled_.find(Key(source_path, definition_hash));
    return it != disabled_.end() && it->second;
}

void HookTrustStore::SetDisabled(const std::string& source_path, const std::string& definition_hash, bool disabled) {
    disabled_[Key(source_path, definition_hash)] = disabled;
    dirty_ = true;
    Save();
}

std::string HookTrustStore::TrustedCommand(const std::string& source_path,
                                           const std::string& definition_hash) const {
    const auto it = trusted_.find(Key(source_path, definition_hash));
    return it == trusted_.end() ? std::string() : it->second.command;
}

std::optional<std::string> HookTrustStore::Save() {
    if (!path_.has_value() || path_->empty() || !dirty_) {
        return std::nullopt;
    }
    nlohmann::json root;
    root["schema_version"] = 1;
    nlohmann::json trusted = nlohmann::json::object();
    for (const auto& [key, entry] : trusted_) {
        trusted[key]["command"] = entry.command;
        trusted[key]["trusted_at"] = entry.trusted_at;
    }
    root["trusted"] = std::move(trusted);
    nlohmann::json disabled = nlohmann::json::object();
    for (const auto& [key, value] : disabled_) {
        disabled[key] = value;
    }
    root["disabled"] = std::move(disabled);

    // 原子写,统一走 platform::AtomicWriteFile(信任账是持久事实;此处只
    // 动落盘机制,不动 Hook 语义——身份权限线另有人在改)。
    const std::filesystem::path target(reinterpret_cast<const char8_t*>(path_->c_str()));
    const auto written = platform::AtomicWriteFile(target, root.dump(2));
    if (!written.has_value()) {
        return "hook 信任账本落盘失败: " + written.error().message;
    }
    dirty_ = false;
    return std::nullopt;
}

}  // namespace lubancode::hooks
