#include "gateway/profile.hpp"

#include <array>
#include <fstream>
#include <optional>
#include <sstream>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"

namespace lubancode::gateway {

bool IsValidGatewayProfileName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    if (name.front() == '.') return false;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    if (name.find("..") != std::string_view::npos) return false;
    return true;
}

GatewayProfilePaths ResolveGatewayProfilePaths(const std::filesystem::path& root,
                                               std::string_view profile_name) {
    GatewayProfilePaths paths;
    if (!IsValidGatewayProfileName(profile_name)) {
        return paths;  // 空 root = 非法,调用方明报
    }
    paths.root = root;
    paths.name = std::string(profile_name);
    paths.profile_dir = root / "profiles" / std::string(profile_name);
    paths.config_file = paths.profile_dir / "gateway.json";
    paths.lock_file = paths.profile_dir / "gateway.lock";
    paths.control_file = paths.profile_dir / "control.json";
    paths.control_dir = paths.profile_dir / "control";
    paths.boot_history = paths.profile_dir / "boot-history.jsonl";
    paths.logs_dir = paths.profile_dir / "logs";
    paths.log_file = paths.logs_dir / "gateway.log";
    return paths;
}

bool GatewayProfileConfig::Validate(std::string* error) const {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (shutdown_grace_secs < 1 || shutdown_grace_secs > 600) {
        return fail("shutdown_grace_secs 须在 1..600 秒");
    }
    if (max_concurrent_sessions < 1 || max_concurrent_sessions > 64) {
        return fail("max_concurrent_sessions 须在 1..64");
    }
    if (safe_mode_threshold < 1 || safe_mode_threshold > 10) {
        return fail("safe_mode_threshold 须在 1..10");
    }
    return true;
}

namespace {

std::optional<GatewayProfileConfig> ParseConfigJson(const nlohmann::json& json,
                                                    std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) *error = "gateway.config_invalid: " + message;
        return std::optional<GatewayProfileConfig>{};
    };
    if (!json.is_object()) return fail("gateway.json 必须是 JSON object");
    // 严格解析:未知字段明报(配置是用户手写的持久合同,多写的字段静默
    // 忽略会把"以为配上了"藏到很晚才发现)。
    const std::array<const char*, 4> kKnown = {"schema_version", "shutdown_grace_secs",
                                               "max_concurrent_sessions", "safe_mode_threshold"};
    for (auto it = json.begin(); it != json.end(); ++it) {
        bool known = false;
        for (const char* key : kKnown) {
            if (it.key() == key) {
                known = true;
                break;
            }
        }
        if (!known) return fail("未知字段 " + it.key());
    }
    GatewayProfileConfig config;
    if (json.contains("schema_version")) {
        if (!json["schema_version"].is_number_integer()) {
            return fail("schema_version 必须是整数");
        }
        config.schema_version = json["schema_version"].get<int>();
        if (config.schema_version != 1) {
            return fail("schema_version 只认 1,不认 " + std::to_string(config.schema_version));
        }
    }
    if (json.contains("shutdown_grace_secs")) {
        if (!json["shutdown_grace_secs"].is_number_integer()) {
            return fail("shutdown_grace_secs 必须是整数");
        }
        config.shutdown_grace_secs = json["shutdown_grace_secs"].get<int>();
    }
    if (json.contains("max_concurrent_sessions")) {
        if (!json["max_concurrent_sessions"].is_number_integer()) {
            return fail("max_concurrent_sessions 必须是整数");
        }
        config.max_concurrent_sessions = json["max_concurrent_sessions"].get<int>();
    }
    if (json.contains("safe_mode_threshold")) {
        if (!json["safe_mode_threshold"].is_number_integer()) {
            return fail("safe_mode_threshold 必须是整数");
        }
        config.safe_mode_threshold = json["safe_mode_threshold"].get<int>();
    }
    std::string validate_error;
    if (!config.Validate(&validate_error)) {
        return fail(validate_error);
    }
    return config;
}

}  // namespace

GatewayConfigLoad LoadGatewayConfig(const std::filesystem::path& config_file) {
    GatewayConfigLoad result;
    std::error_code ec;
    if (!std::filesystem::exists(config_file, ec) || ec) {
        result.status = GatewayConfigLoad::Status::Missing;
        return result;
    }
    std::ifstream stream(config_file, std::ios::binary);
    if (!stream) {
        result.status = GatewayConfigLoad::Status::Invalid;
        result.error = "gateway.config_invalid: gateway.json 在,但打不开(" +
                       platform::PathToUtf8(config_file) + ")";
        return result;
    }
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        result.status = GatewayConfigLoad::Status::Invalid;
        result.error = std::string("gateway.config_invalid: gateway.json 不是合法 JSON: ") + e.what();
        return result;
    }
    std::string parse_error;
    auto config = ParseConfigJson(parsed, &parse_error);
    if (!config.has_value()) {
        result.status = GatewayConfigLoad::Status::Invalid;
        result.error = parse_error;
        return result;
    }
    result.status = GatewayConfigLoad::Status::Ok;
    result.config = *config;
    return result;
}

}  // namespace lubancode::gateway
