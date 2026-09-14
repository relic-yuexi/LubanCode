// im_preferences.hpp 的实现。schema 1:{"schema":1,"last":{"channel":..,
// "account":..},"recent_account":{"qqbot":"main"}}。只存标识。
#include "config/im_preferences.hpp"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "config/config.hpp"           // HomeLubancodeDir
#include "platform/atomic_write.hpp"   // 原子保存
#include "platform/paths.hpp"

namespace lubancode::config {

namespace {

constexpr int kImPreferencesSchema = 1;

}  // namespace

ImPreferenceStore::ImPreferenceStore(std::filesystem::path file) : file_(std::move(file)) {}

std::optional<std::filesystem::path> ImPreferenceStore::DefaultFilePath() {
    const auto home = HomeLubancodeDir();
    if (!home.has_value()) {
        return std::nullopt;
    }
    return platform::Utf8ToPath(*home) / "im-preferences.json";
}

ImPreferences ImPreferenceStore::Load() const {
    ImPreferences preferences;
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec) || ec) {
        return preferences;  // 没档 = 空偏好,正常首启
    }
    std::ifstream in(file_, std::ios::binary);
    if (!in.is_open()) {
        return preferences;  // 读不开:当没配过,不拦启动
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    try {
        const nlohmann::json root = nlohmann::json::parse(buffer.str());
        if (!root.is_object() || !root.contains("schema") || !root["schema"].is_number_integer() ||
            root["schema"].get<int>() != kImPreferencesSchema) {
            return preferences;  // 版本不认:空偏好,不猜旧格式
        }
        if (root.contains("last") && root["last"].is_object()) {
            const nlohmann::json& last = root["last"];
            if (last.contains("channel") && last["channel"].is_string() &&
                last.contains("account") && last["account"].is_string()) {
                ImRecentSelection selection;
                selection.channel_id = last["channel"].get<std::string>();
                selection.account_id = last["account"].get<std::string>();
                if (selection.present()) {
                    preferences.last = selection;
                }
            }
        }
        if (root.contains("recent_account") && root["recent_account"].is_object()) {
            for (auto it = root["recent_account"].begin(); it != root["recent_account"].end();
                 ++it) {
                if (it.value().is_string()) {
                    preferences.recent_account[it.key()] = it.value().get<std::string>();
                }
            }
        }
    } catch (const nlohmann::json::parse_error&) {
        return preferences;  // 坏档:空偏好(§6.2 偏好丢失只回到选择流程)
    }
    return preferences;
}

std::optional<std::string> ImPreferenceStore::Save(const ImPreferences& preferences) const {
    nlohmann::json root;
    root["schema"] = kImPreferencesSchema;
    if (preferences.last.has_value() && preferences.last->present()) {
        root["last"] = nlohmann::json{{"channel", preferences.last->channel_id},
                                      {"account", preferences.last->account_id}};
    }
    if (!preferences.recent_account.empty()) {
        root["recent_account"] = preferences.recent_account;
    }
    const auto written = platform::AtomicWriteFile(
        file_, root.dump(2) + "\n", platform::WriteDurability::ProcessCrashDurability);
    if (!written.has_value()) {
        return std::unexpected(written.error().code + ": " + written.error().message);
    }
    return {};
}

}  // namespace lubancode::config
