// 渠道配对控制命令实现:合同见 hpp 注释(文件面与 stop.json 同款纪律)。
#include "gateway/pairing_command.hpp"

#include <fstream>

#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1)
#include "platform/paths.hpp"

namespace lubancode::gateway {

namespace {

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

// 命令文件名:pairing-<command_id>.json(结果文件带 .result,轮询侧要跳过)。
std::filesystem::path CommandFile(const std::filesystem::path& control_dir,
                                  const std::string& command_id) {
    return control_dir / ("pairing-" + command_id + ".json");
}

std::filesystem::path ResultFile(const std::filesystem::path& control_dir,
                                 const std::string& command_id) {
    return control_dir / ("pairing-" + command_id + ".result.json");
}

// "pairing-<id>.json" 的文件名拆 id;结果文件(.result.json)与其它名字
// 回空——结果文件同样以 .json 结尾,须显式排除,否则命令轮询会误吃回执。
std::string CommandIdFromFilename(const std::string& filename) {
    const std::string prefix = "pairing-";
    const std::string suffix = ".json";
    const std::string result_suffix = ".result.json";
    if (filename.size() <= prefix.size() + suffix.size()) return std::string();
    if (filename.rfind(prefix, 0) != 0) return std::string();
    if (filename.size() >= result_suffix.size() &&
        filename.compare(filename.size() - result_suffix.size(), result_suffix.size(),
                         result_suffix) == 0) {
        return std::string();  // 结果文件:不归命令轮询
    }
    if (filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return std::string();
    }
    return filename.substr(prefix.size(), filename.size() - prefix.size() - suffix.size());
}

}  // namespace

nlohmann::json GatewayPairingCommand::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema_version"] = schema_version;
    json["boot_id"] = boot_id;
    json["command_id"] = command_id;
    json["action"] = action;
    json["channel_id"] = channel_id;
    json["account_id"] = account_id;
    json["token"] = token;
    json["requested_at_ms"] = requested_at_ms;
    return json;
}

std::optional<GatewayPairingCommand> GatewayPairingCommand::FromJson(const nlohmann::json& json,
                                                                     std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) *error = message;
        return std::optional<GatewayPairingCommand>{};
    };
    if (!json.is_object()) return fail("配对命令必须是 JSON object");
    const auto require_string = [&json](const char* key, std::string* out) {
        if (!json.contains(key) || !json[key].is_string()) return false;
        *out = json[key].get<std::string>();
        return true;
    };
    GatewayPairingCommand command;
    if (!require_string("boot_id", &command.boot_id)) return fail("缺 boot_id 或不是字符串");
    if (!require_string("command_id", &command.command_id)) return fail("缺 command_id");
    if (!IsValidPairingCommandId(command.command_id)) return fail("command_id 不合法");
    if (!require_string("action", &command.action)) return fail("缺 action");
    if (command.action != "approve" && command.action != "reject") {
        return fail("action 只认 approve|reject: " + command.action);
    }
    if (!require_string("channel_id", &command.channel_id)) return fail("缺 channel_id");
    if (!require_string("account_id", &command.account_id)) return fail("缺 account_id");
    if (!require_string("token", &command.token)) return fail("缺 token");
    if (json.contains("schema_version") && json["schema_version"].is_number_integer()) {
        command.schema_version = json["schema_version"].get<int>();
    }
    if (json.contains("requested_at_ms") && json["requested_at_ms"].is_number_integer()) {
        command.requested_at_ms = json["requested_at_ms"].get<std::int64_t>();
    }
    return command;
}

bool IsValidPairingCommandId(const std::string& command_id) {
    if (command_id.empty() || command_id.size() > 64) return false;
    for (const char c : command_id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string WritePairingCommand(const std::filesystem::path& control_dir,
                                const GatewayPairingCommand& command) {
    if (!IsValidPairingCommandId(command.command_id)) {
        return "command_id 不合法(单段 [A-Za-z0-9-],≤64): " + command.command_id;
    }
    std::error_code ec;
    std::filesystem::create_directories(control_dir, ec);
    if (ec) return "建控制命令目录失败 " + platform::PathToUtf8(control_dir) + ": " + ec.message();
    return AtomicWriteText(CommandFile(control_dir, command.command_id), command.ToJson().dump());
}

std::vector<GatewayPairingCommand> PollPairingCommands(const std::filesystem::path& control_dir,
                                                       const std::string& current_boot_id) {
    std::vector<GatewayPairingCommand> commands;
    std::error_code ec;
    if (!std::filesystem::is_directory(control_dir, ec) || ec) {
        return commands;
    }
    for (const auto& entry : std::filesystem::directory_iterator(control_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) continue;
        const std::string command_id =
            CommandIdFromFilename(entry.path().filename().string());
        if (command_id.empty()) continue;  // 结果文件/无关文件:不消费不删
        // 读到就删(消费即取走):boot_id 对不上也删——那是上一只实例的
        // 陈旧命令,不追新实例。读不懂的删掉(原子写之下只剩竞态尾巴)。
        const std::string text = ReadTextFile(entry.path());
        std::error_code remove_ec;
        std::filesystem::remove(entry.path(), remove_ec);
        if (text.empty()) continue;
        try {
            const nlohmann::json parsed = nlohmann::json::parse(text);
            std::string parse_error;
            auto command = GatewayPairingCommand::FromJson(parsed, &parse_error);
            if (!command.has_value()) continue;
            if (!command->boot_id.empty() && command->boot_id != current_boot_id) continue;
            commands.push_back(std::move(*command));
        } catch (const nlohmann::json::exception&) {
            // 坏文件已删,下一拍不再读它。
        }
    }
    return commands;
}

nlohmann::json GatewayPairingCommandResult::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema_version"] = schema_version;
    json["command_id"] = command_id;
    json["action"] = action;
    json["ok"] = ok;
    json["sender_id"] = sender_id;
    json["error"] = error;
    json["detail"] = detail;
    return json;
}

std::optional<GatewayPairingCommandResult> GatewayPairingCommandResult::FromJson(
    const nlohmann::json& json, std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) *error = message;
        return std::optional<GatewayPairingCommandResult>{};
    };
    if (!json.is_object()) return fail("配对命令结果必须是 JSON object");
    const auto require_string = [&json](const char* key, std::string* out) {
        if (!json.contains(key) || !json[key].is_string()) return false;
        *out = json[key].get<std::string>();
        return true;
    };
    GatewayPairingCommandResult result;
    if (!require_string("command_id", &result.command_id)) return fail("缺 command_id");
    if (!require_string("action", &result.action)) return fail("缺 action");
    if (!require_string("sender_id", &result.sender_id)) return fail("缺 sender_id");
    if (!require_string("error", &result.error)) return fail("缺 error");
    if (!require_string("detail", &result.detail)) return fail("缺 detail");
    if (json.contains("ok") && json["ok"].is_boolean()) {
        result.ok = json["ok"].get<bool>();
    }
    if (json.contains("schema_version") && json["schema_version"].is_number_integer()) {
        result.schema_version = json["schema_version"].get<int>();
    }
    return result;
}

std::string WritePairingCommandResult(const std::filesystem::path& control_dir,
                                      const GatewayPairingCommandResult& result) {
    if (!IsValidPairingCommandId(result.command_id)) {
        return "command_id 不合法(单段 [A-Za-z0-9-],≤64): " + result.command_id;
    }
    std::error_code ec;
    std::filesystem::create_directories(control_dir, ec);
    if (ec) return "建控制命令目录失败 " + platform::PathToUtf8(control_dir) + ": " + ec.message();
    return AtomicWriteText(ResultFile(control_dir, result.command_id), result.ToJson().dump());
}

std::optional<GatewayPairingCommandResult> TakePairingCommandResult(
    const std::filesystem::path& control_dir, const std::string& command_id,
    std::string* error) {
    if (error != nullptr) error->clear();
    if (!IsValidPairingCommandId(command_id)) {
        if (error != nullptr) *error = "command_id 不合法: " + command_id;
        return std::nullopt;
    }
    const std::filesystem::path file = ResultFile(control_dir, command_id);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        return std::nullopt;  // 还没回:CLI 继续轮询
    }
    const std::string text = ReadTextFile(file);
    std::error_code remove_ec;
    std::filesystem::remove(file, remove_ec);  // 读走即删,不留陈旧结果
    if (text.empty()) {
        if (error != nullptr) *error = "结果文件是空的";
        return std::nullopt;
    }
    try {
        const nlohmann::json parsed = nlohmann::json::parse(text);
        std::string parse_error;
        auto result = GatewayPairingCommandResult::FromJson(parsed, &parse_error);
        if (!result.has_value() && error != nullptr) {
            *error = parse_error;
        }
        return result;
    } catch (const nlohmann::json::exception& e) {
        if (error != nullptr) {
            *error = std::string("结果文件不是合法 JSON: ") + e.what();
        }
        return std::nullopt;
    }
}

}  // namespace lubancode::gateway
