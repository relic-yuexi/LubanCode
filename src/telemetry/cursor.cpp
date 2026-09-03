// Telemetry 投影 cursor 的实现。合同见 cursor.hpp 文件头。
#include "telemetry/cursor.hpp"

#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "telemetry/contract.hpp"
#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1)
#include "platform/paths.hpp"

namespace lubancode::telemetry {
namespace {

// 单段名校验:目录/文件名只认 [A-Za-z0-9._-](与 trajectory 目录同规矩),
// 拒路径分隔符与 ".." 一类可逃逸材料。workspace_key 是哈希形、session_id
// 是时间戳形,天然过验;不过验的当场拒绝,不猜。
bool IsValidSingleSegment(std::string_view name) {
    if (name.empty() || name.size() > 128 || name == "." || name == "..") {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool WriteTextFileAtomic(const std::filesystem::path& path, const std::string& content) {
    // 统一原子写(审计 P1):唯一临时名 + 平台原子替换,替掉本文件原先
    // 自备的固定 .tmp 协议。耐久档与旧实现持平(可见性原子)。
    return platform::AtomicWriteFile(path, content).has_value();
}

}  // namespace

std::string CursorFileStem(std::string_view stream) {
    std::string stem(stream);
    for (char& c : stem) {
        if (c == '/' || c == '\\') {
            c = '_';
        }
    }
    return stem;
}

std::filesystem::path CursorFilePath(const std::filesystem::path& cursors_root,
                                     std::string_view workspace_key, std::string_view session_id,
                                     std::string_view stream) {
    return cursors_root / std::string(workspace_key) / std::string(session_id) /
           (CursorFileStem(stream) + ".json");
}

std::optional<StreamCursor> LoadCursor(const std::filesystem::path& cursors_root,
                                       std::string_view workspace_key,
                                       std::string_view session_id, std::string_view stream,
                                       std::string* error_code) {
    if (error_code != nullptr) {
        error_code->clear();
    }
    if (!IsValidSingleSegment(workspace_key) || !IsValidSingleSegment(session_id)) {
        if (error_code != nullptr) {
            *error_code = "telemetry.cursor_bad_identity";
        }
        return std::nullopt;
    }
    const std::filesystem::path path =
        CursorFilePath(cursors_root, workspace_key, session_id, stream);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return std::nullopt;  // 新 stream:从 Journal 头投
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        if (error_code != nullptr) {
            *error_code = "telemetry.io_error";
        }
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const nlohmann::json json = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        if (error_code != nullptr) {
            *error_code = "telemetry.cursor_corrupt";
        }
        return std::nullopt;
    }
    const auto read_string = [&](const char* key, std::string* out) {
        return json.contains(key) && json.at(key).is_string() &&
               (*out = json.at(key).get<std::string>(), true);
    };
    StreamCursor cursor;
    std::string schema;
    int version = 0;
    if (!read_string("schema", &schema) || schema != kCursorSchema ||
        !json.contains("version") || !json.at("version").is_number_integer() ||
        (version = json.at("version").get<int>()) != kCursorVersion) {
        if (error_code != nullptr) {
            *error_code = "telemetry.cursor_schema_mismatch";
        }
        return std::nullopt;
    }
    if (!read_string("workspace_key", &cursor.workspace_key) ||
        !read_string("session_id", &cursor.session_id) || !read_string("stream", &cursor.stream)) {
        if (error_code != nullptr) {
            *error_code = "telemetry.cursor_corrupt";
        }
        return std::nullopt;
    }
    // 请求的身份与文件身份不一致 = 换账,拒绝(§14.2"stream 换账,停止并报错")。
    if (cursor.workspace_key != workspace_key || cursor.session_id != session_id ||
        cursor.stream != stream) {
        if (error_code != nullptr) {
            *error_code = "telemetry.cursor_identity_mismatch";
        }
        return std::nullopt;
    }
    read_string("last_event_id", &cursor.last_event_id);
    read_string("last_event_hash", &cursor.last_event_hash);
    read_string("projector_version", &cursor.projector_version);
    if (json.contains("projection_generation") &&
        json.at("projection_generation").is_number_integer()) {
        cursor.projection_generation = json.at("projection_generation").get<int>();
    }
    if (json.contains("updated_at_ms") && json.at("updated_at_ms").is_number_integer()) {
        cursor.updated_at_ms = json.at("updated_at_ms").get<std::int64_t>();
    }
    return cursor;
}

bool StoreCursor(const std::filesystem::path& cursors_root, const StreamCursor& cursor) {
    if (!IsValidSingleSegment(cursor.workspace_key) || !IsValidSingleSegment(cursor.session_id)) {
        return false;
    }
    nlohmann::json json = nlohmann::json::object();
    json["schema"] = kCursorSchema;
    json["version"] = kCursorVersion;
    json["workspace_key"] = cursor.workspace_key;
    json["session_id"] = cursor.session_id;
    json["stream"] = cursor.stream;
    json["last_event_id"] = cursor.last_event_id;
    json["last_event_hash"] = cursor.last_event_hash;
    json["projector_version"] = cursor.projector_version;
    json["projection_generation"] = cursor.projection_generation;
    json["updated_at_ms"] = cursor.updated_at_ms;

    std::error_code ec;
    std::filesystem::create_directories(
        CursorFilePath(cursors_root, cursor.workspace_key, cursor.session_id, cursor.stream)
            .parent_path(),
        ec);
    if (ec) {
        return false;
    }
    return WriteTextFileAtomic(
        CursorFilePath(cursors_root, cursor.workspace_key, cursor.session_id, cursor.stream),
        json.dump());
}

}  // namespace lubancode::telemetry
