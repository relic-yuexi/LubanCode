// 实现说明见 settings_local.hpp。函数体自 config.cpp 原样搬来,行为
// 一字未改(骨架拆解反弹·问题 7 纯搬家)。
#include "config/settings_local.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"

namespace lubancode::config {

std::string SettingsLocalPath(const std::string& cwd_dir) {
    // cwd_dir 是 UTF-8(CurrentDirUtf8 来的),窄口构造 path 会按 ACP 误解
    // 中文/emoji 字节——一律走 u8 通道。
    return platform::PathToUtf8(platform::Utf8ToPath(cwd_dir) / ".lubancode" / "settings.local.json");
}

std::expected<SettingsLocal, std::string> ParseSettingsLocal(const std::string& json_text,
                                                              const std::string& path_for_error) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::parse_error& e) {
        return std::unexpected("settings.local.json " + path_for_error + " 不是合法 JSON: " + e.what());
    }
    if (!parsed.is_object()) {
        return std::unexpected("settings.local.json " + path_for_error + " 顶层必须是一个 JSON object");
    }

    SettingsLocal out;
    if (!parsed.contains("permissions")) {
        return out;  // 没有 permissions 段就是空的,不算错
    }
    const auto& perms = parsed["permissions"];
    if (!perms.is_object()) {
        return std::unexpected("settings.local.json " + path_for_error + " 里的 permissions 字段必须是 JSON object");
    }

    // 字符串数组:非字符串元素跳过(宽容),不因为夹了个坏元素就整份作废。
    const auto read_str_array = [&](const char* key, std::vector<std::string>& into) {
        if (!perms.contains(key) || !perms[key].is_array()) {
            return;
        }
        for (const auto& item : perms[key]) {
            if (item.is_string()) {
                into.push_back(item.get<std::string>());
            }
        }
    };
    read_str_array("allow_tools", out.allow_tools);
    read_str_array("allow_commands", out.allow_commands);
    read_str_array("deny_commands", out.deny_commands);

    if (perms.contains("default_confirm_mode") && perms["default_confirm_mode"].is_string()) {
        std::string mode = perms["default_confirm_mode"].get<std::string>();
        if (!mode.empty()) {
            out.default_confirm_mode = std::move(mode);  // auto/yolo/confirm,别的值交给调用方判
        }
    }
    // Plan 模式单:起手协作档。plan/default 之外的值交给调用方判(RunCli
    // 明报到 stderr)。
    if (perms.contains("default_collaboration_mode") && perms["default_collaboration_mode"].is_string()) {
        std::string mode = perms["default_collaboration_mode"].get<std::string>();
        if (!mode.empty()) {
            out.default_collaboration_mode = std::move(mode);
        }
    }
    return out;
}

std::expected<std::optional<SettingsLocal>, std::string> LoadSettingsLocal(const std::string& cwd_dir) {
    const std::string path = SettingsLocalPath(cwd_dir);
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(path), ec) || ec) {
        return std::optional<SettingsLocal>(std::nullopt);  // 没这文件不算错
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected("settings.local.json " + path + " 存在,但打不开(检查一下权限)");
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    auto parsed = ParseSettingsLocal(buffer.str(), path);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    return std::optional<SettingsLocal>(*parsed);
}

std::expected<std::string, std::string> AddAllowedToolToSettingsLocal(const std::string& cwd_dir,
                                                                       const std::string& tool_name) {
    namespace fs = std::filesystem;
    const std::string path = SettingsLocalPath(cwd_dir);

    // 已有内容原样读进来(保留不认得的字段);读不到/坏 JSON 就从空 object 起。
    nlohmann::json root = nlohmann::json::object();
    {
        std::error_code ec;
        if (fs::exists(fs::path(path), ec) && !ec) {
            std::ifstream in(path, std::ios::binary);
            if (in.is_open()) {
                std::ostringstream buffer;
                buffer << in.rdbuf();
                try {
                    auto existing = nlohmann::json::parse(buffer.str());
                    if (existing.is_object()) {
                        root = std::move(existing);
                    }
                } catch (const nlohmann::json::parse_error&) {
                    // 坏 JSON:不覆盖用户手写的东西,报错让人自己看一眼。
                    return std::unexpected("settings.local.json " + path +
                                            " 不是合法 JSON,没敢覆盖;请手动检查后再试");
                }
            }
        }
    }

    if (!root.contains("permissions") || !root["permissions"].is_object()) {
        root["permissions"] = nlohmann::json::object();
    }
    auto& perms = root["permissions"];
    if (!perms.contains("allow_tools") || !perms["allow_tools"].is_array()) {
        perms["allow_tools"] = nlohmann::json::array();
    }
    auto& allow = perms["allow_tools"];
    for (const auto& item : allow) {
        if (item.is_string() && item.get<std::string>() == tool_name) {
            return path;  // 已经在了,幂等,不重复写
        }
    }
    allow.push_back(tool_name);

    // 项目级 .lubancode/ 只在这一刻按需落地。
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) {
        return std::unexpected("建目录 " + platform::PathToUtf8(fs::path(path).parent_path()) + " 失败: " + ec.message());
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return std::unexpected("settings.local.json " + path + " 打不开写入(检查一下权限)");
    }
    out << root.dump(2);
    if (!out.good()) {
        return std::unexpected("settings.local.json " + path + " 写入失败(检查一下磁盘/权限)");
    }
    return path;
}

std::string EnsureGitignoreCoversSettingsLocal(const std::string& cwd_dir) {
    namespace fs = std::filesystem;
    const fs::path gitignore = fs::path(cwd_dir) / ".gitignore";
    const std::string kIgnoreLine = ".lubancode/settings.local.json";

    std::error_code ec;
    if (!fs::exists(gitignore, ec) || ec) {
        // 没有 .gitignore,别硬塞——打一行提示教用户手动加。
        return "提示:本目录没有 .gitignore;要不进版本库,请手动加一行 " + kIgnoreLine;
    }

    std::string content;
    {
        std::ifstream in(gitignore, std::ios::binary);
        if (!in.is_open()) {
            return "提示:.gitignore 打不开;请手动加一行 " + kIgnoreLine;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        content = buffer.str();
    }

    // 已经挡住?整个 .lubancode/ 目录被忽略、或者精确忽略了这个文件,都算。
    if (content.find(".lubancode/") != std::string::npos ||
        content.find("settings.local.json") != std::string::npos) {
        return "";  // 已经挡住,什么都不必做
    }

    std::ofstream out(gitignore, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        return "提示:.gitignore 追加不了;请手动加一行 " + kIgnoreLine;
    }
    if (!content.empty() && content.back() != '\n') {
        out << "\n";
    }
    out << kIgnoreLine << "\n";
    return "已在 .gitignore 追加一行 " + kIgnoreLine;
}

}  // namespace lubancode::config
