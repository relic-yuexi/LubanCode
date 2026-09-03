#include "gateway/control_server.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1:旧写法替换失败会先删正式文件)
#include "platform/paths.hpp"

namespace lubancode::gateway {

namespace {

// 原子换代写,统一走 platform::AtomicWriteFile(§8.2"spec 可原子换代"):
// 唯一临时名、平台原子替换、失败不删正式文件、结构化错误。控制快照不是
// append 账,是可换代的投影;但事实事件(boot history)仍只追加。
std::string AtomicWriteText(const std::filesystem::path& target, const std::string& text) {
    const auto result = platform::AtomicWriteFile(target, text);
    if (!result.has_value()) {
        return result.error().code + ": " + result.error().message;
    }
    return std::string();
}

std::string ReadTextFile(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return std::string();
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

}  // namespace

nlohmann::json GatewayControlSnapshot::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema_version"] = schema_version;
    json["profile"] = profile;
    json["boot_id"] = boot_id;
    json["pid"] = pid;
    json["start_token"] = start_token;
    json["started_at_ms"] = started_at_ms;
    json["state"] = state;
    json["health"] = health;
    json["safe_mode"] = safe_mode;
    json["last_shutdown"] = last_shutdown;
    json["version"] = version;
    json["updated_at_ms"] = updated_at_ms;
    return json;
}

std::optional<GatewayControlSnapshot> GatewayControlSnapshot::FromJson(const nlohmann::json& json,
                                                                       std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) *error = message;
        return std::optional<GatewayControlSnapshot>{};
    };
    if (!json.is_object()) return fail("control.json 必须是 JSON object");
    // 读侧只严查读得到的必填字段;未知字段忽略(前向兼容)。
    const auto require_string = [&](const char* key, std::string* out) {
        if (!json.contains(key) || !json[key].is_string()) {
            *out = std::string();
            return false;
        }
        *out = json[key].get<std::string>();
        return true;
    };
    GatewayControlSnapshot snapshot;
    std::string scratch;
    if (!require_string("state", &snapshot.state)) return fail("缺 state 或不是字符串");
    require_string("profile", &snapshot.profile);
    require_string("boot_id", &snapshot.boot_id);
    require_string("start_token", &snapshot.start_token);
    require_string("health", &snapshot.health);
    require_string("last_shutdown", &snapshot.last_shutdown);
    require_string("version", &snapshot.version);
    if (json.contains("pid") && json["pid"].is_number_integer()) {
        snapshot.pid = static_cast<unsigned long>(json["pid"].get<std::int64_t>());
    }
    if (json.contains("schema_version") && json["schema_version"].is_number_integer()) {
        snapshot.schema_version = json["schema_version"].get<int>();
    }
    if (json.contains("started_at_ms") && json["started_at_ms"].is_number_integer()) {
        snapshot.started_at_ms = json["started_at_ms"].get<std::int64_t>();
    }
    if (json.contains("updated_at_ms") && json["updated_at_ms"].is_number_integer()) {
        snapshot.updated_at_ms = json["updated_at_ms"].get<std::int64_t>();
    }
    if (json.contains("safe_mode") && json["safe_mode"].is_boolean()) {
        snapshot.safe_mode = json["safe_mode"].get<bool>();
    }
    return snapshot;
}

std::string WriteControlSnapshot(const std::filesystem::path& control_file,
                                 const GatewayControlSnapshot& snapshot) {
    return AtomicWriteText(control_file, snapshot.ToJson().dump());
}

std::optional<GatewayControlSnapshot> ReadControlSnapshot(const std::filesystem::path& control_file,
                                                          std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(control_file, ec) || ec) {
        if (error != nullptr) error->clear();
        return std::nullopt;  // 没有快照文件:不是错
    }
    const std::string text = ReadTextFile(control_file);
    if (text.empty()) {
        if (error != nullptr) *error = "control.json 是空的";
        return std::nullopt;
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        if (error != nullptr) {
            *error = std::string("control.json 不是合法 JSON(可能写了一半): ") + e.what();
        }
        return std::nullopt;
    }
    std::string parse_error;
    auto snapshot = GatewayControlSnapshot::FromJson(parsed, &parse_error);
    if (!snapshot.has_value() && error != nullptr) {
        *error = parse_error;
    }
    return snapshot;
}

nlohmann::json GatewayStopCommand::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema_version"] = schema_version;
    json["boot_id"] = boot_id;
    json["requested_at_ms"] = requested_at_ms;
    return json;
}

std::string WriteStopCommand(const std::filesystem::path& control_dir,
                             const GatewayStopCommand& command) {
    std::error_code ec;
    std::filesystem::create_directories(control_dir, ec);
    if (ec) return "建控制命令目录失败 " + platform::PathToUtf8(control_dir) + ": " + ec.message();
    return AtomicWriteText(control_dir / "stop.json", command.ToJson().dump());
}

bool PollStopCommand(const std::filesystem::path& control_dir, const std::string& current_boot_id) {
    const std::filesystem::path command_file = control_dir / "stop.json";
    std::error_code ec;
    if (!std::filesystem::exists(command_file, ec) || ec) {
        return false;
    }
    // 读到就删(消费即取走):boot_id 对不上也删——那是上一只实例的陈旧
    // 命令,不追杀新实例。
    bool for_us = false;
    const std::string text = ReadTextFile(command_file);
    if (!text.empty()) {
        try {
            const nlohmann::json parsed = nlohmann::json::parse(text);
            if (parsed.is_object() && parsed.contains("boot_id") && parsed["boot_id"].is_string()) {
                const std::string target = parsed["boot_id"].get<std::string>();
                for_us = target.empty() || target == current_boot_id;
            }
        } catch (const nlohmann::json::exception&) {
            // 命令文件写了一半:删掉。外部 CLI 是原子写,真到这里只剩竞态
            // 尾巴;不删会让主循环每拍重读一次坏文件。
        }
    }
    std::filesystem::remove(command_file, ec);
    return for_us;
}

}  // namespace lubancode::gateway
