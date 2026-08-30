#include "trajectory/directory.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "hooks/hash.hpp"
#include "platform/paths.hpp"

namespace lubancode::trajectory {
namespace {

std::string TwoDigits(int value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02d", value);
    return buffer;
}

// 单段名校验(§12.1):目录/文件名只认 [A-Za-z0-9._-],拒绝路径分隔符、
// ".."、"。"盘符冒号一类可逃逸材料。
bool IsValidSingleSegment(std::string_view name) {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    if (name == "." || name == "..") {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        c == '-' || c == '_' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool WriteTextFileAtomic(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";  // 纯 ASCII 后缀,窄口拼接不涉代码页
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return false;
        }
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.flush();
        if (!file.good()) {
            file.close();
            std::error_code ignored;
            std::filesystem::remove(tmp, ignored);
            return false;
        }
    }
    return platform::ReplaceFileAtomically(tmp, path).has_value();
}

}  // namespace

std::string NormalizeRootPathText(const std::filesystem::path& root) {
    std::filesystem::path normalized = std::filesystem::absolute(root).lexically_normal();
    std::string text = platform::PathToUtf8(normalized);
    // 统一正斜杠、去尾斜杠。Windows 文件系统大小写不敏感:整串折叠 ASCII
    // 小写,免得 D:/Work 与 d:/work 各立一间 workspace。POSIX 大小写敏感,
    // 保持原样。
    for (char& c : text) {
        if (c == '\\') {
            c = '/';
        }
#ifdef _WIN32
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
#endif
    }
    if (text.size() > 1 && text.back() == '/') {
        text.pop_back();
    }
    return text;
}

std::string ComputeWorkspaceKey(const std::filesystem::path& root) {
    const std::string normalized = NormalizeRootPathText(root);
    // basename 一律从规范化文本切最后一段非空路径,不问平台 fs 语义——
    // POSIX 把反斜杠当普通字符,fs::path("D:\\work\\demo").filename() 在
    // macOS/Linux 上是整串反斜杠串,workspace_key 就成了"整路径-hash"
    // (CI 实翻)。规范文本已统一正斜杠,两边同口径。
    std::string basename;
    std::istringstream stream(normalized);
    std::string segment;
    while (std::getline(stream, segment, '/')) {
        if (!segment.empty() && segment != "." && segment != "..") {
            basename = segment;
        }
    }
    if (basename.empty()) {
        basename = "root";
    }
    const std::string hash = hooks::Sha256Hex(normalized);
    return basename + "-" + hash.substr(0, 12);
}

std::optional<std::filesystem::path> FindWorkspaceRoot(const std::filesystem::path& cwd) {
    std::error_code ec;
    std::filesystem::path current = std::filesystem::absolute(cwd, ec);
    if (ec) {
        return std::nullopt;
    }
    while (true) {
        if (std::filesystem::exists(current / ".git", ec)) {
            return current;
        }
        if (ec) {
            return std::nullopt;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            return std::nullopt;  // 到根了还没见 .git
        }
        current = parent;
    }
}

std::string GenerateSessionId(int year, int month, int day, int hour, int minute, int second,
                              std::string_view random6) {
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d", year, month, day, hour,
                  minute, second);
    return std::string(stamp) + "-" + std::string(random6);
}

nlohmann::json SessionManifest::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema_version"] = schema_version;
    json["workspace_key"] = workspace_key;
    json["session_id"] = session_id;
    json["launch_cwd"] = launch_cwd;
    json["main_run_id"] = main_run_id;
    json["start_reason"] = start_reason;
    json["previous_session_id"] = previous_session_id.has_value() ? nlohmann::json(*previous_session_id)
                                                                  : nlohmann::json(nullptr);
    json["status"] = status;
    json["created_at_ms"] = created_at_ms;
    json["lubancode_version"] = lubancode_version;
    return json;
}

std::optional<SessionManifest> SessionManifest::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    SessionManifest manifest;
    if (!json.contains("schema_version") || !json.at("schema_version").is_number_integer()) {
        return std::nullopt;
    }
    manifest.schema_version = json.at("schema_version").get<int>();
    if (manifest.schema_version != 1) {
        return std::nullopt;
    }
    const auto read_string = [&](const char* key, std::string* out) {
        return json.contains(key) && json.at(key).is_string() &&
               (*out = json.at(key).get<std::string>(), true);
    };
    if (!read_string("workspace_key", &manifest.workspace_key) ||
        !read_string("session_id", &manifest.session_id) ||
        !read_string("main_run_id", &manifest.main_run_id) ||
        !read_string("status", &manifest.status)) {
        return std::nullopt;
    }
    read_string("launch_cwd", &manifest.launch_cwd);
    read_string("start_reason", &manifest.start_reason);
    read_string("lubancode_version", &manifest.lubancode_version);
    if (json.contains("previous_session_id") && json.at("previous_session_id").is_string()) {
        manifest.previous_session_id = json.at("previous_session_id").get<std::string>();
    }
    if (json.contains("created_at_ms") && json.at("created_at_ms").is_number_integer()) {
        manifest.created_at_ms = json.at("created_at_ms").get<std::int64_t>();
    }
    return manifest;
}

std::expected<void, std::string> WriteSessionJsonAtomic(const std::filesystem::path& session_dir,
                                                        const SessionManifest& manifest) {
    const std::filesystem::path path = session_dir / "session.json";
    const std::string content = manifest.ToJson().dump();
    if (!WriteTextFileAtomic(path, content)) {
        return std::unexpected("session.json 原子写失败: " + platform::PathToUtf8(path));
    }
    return {};
}

std::optional<SessionManifest> ReadSessionJson(const std::filesystem::path& session_dir) {
    const std::filesystem::path path = session_dir / "session.json";
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const auto json = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (json.is_discarded()) {
        return std::nullopt;
    }
    return SessionManifest::FromJson(json);
}

std::expected<TrajectoryDirectory, std::string> TrajectoryDirectory::CreateWorkspace(
    const std::filesystem::path& trajectories_root, const std::filesystem::path& workspace_root,
    const std::string& readable_name, std::int64_t created_at_ms) {
    const std::string key = ComputeWorkspaceKey(workspace_root);
    const std::filesystem::path workspace_dir = trajectories_root / "workspaces" /
                                                 platform::Utf8ToPath(key);
    std::error_code ec;
    for (const char* sub : {"sessions", "lifecycle", "tombstones"}) {
        std::filesystem::create_directories(workspace_dir / sub, ec);
        if (ec) {
            return std::unexpected("workspace 目录建不起: " + platform::PathToUtf8(workspace_dir / sub) +
                                   ": " + ec.message());
        }
    }
    const std::filesystem::path manifest_path = workspace_dir / "workspace.json";
    if (!std::filesystem::exists(manifest_path, ec)) {
        nlohmann::json manifest = nlohmann::json::object();
        manifest["schema_version"] = 1;
        manifest["workspace_key"] = key;
        manifest["root_path"] = NormalizeRootPathText(workspace_root);
        manifest["readable_name"] = readable_name;
        manifest["created_at_ms"] = created_at_ms;
        manifest["git_remote_redacted"] = nullptr;  // 接线批次再填(§3.2)
        if (!WriteTextFileAtomic(manifest_path, manifest.dump())) {
            return std::unexpected("workspace.json 原子写失败: " +
                                   platform::PathToUtf8(manifest_path));
        }
    }
    TrajectoryDirectory directory;
    directory.workspace_dir_ = workspace_dir;
    return directory;
}

std::expected<TrajectoryDirectory, std::string> TrajectoryDirectory::CreateSession(
    const std::filesystem::path& workspaces_root, const std::string& workspace_key,
    const SessionManifest& manifest) {
    if (!IsValidSingleSegment(workspace_key) || !IsValidSingleSegment(manifest.session_id)) {
        return std::unexpected("workspace_key/session_id 不是合法单段名");
    }
    if (manifest.session_id.size() < 8 || manifest.session_id.find('-') == std::string::npos) {
        return std::unexpected("session_id 形状不合(YYYYMMDD-HHMMSS-XXXXXX)");
    }
    const std::filesystem::path workspace_dir =
        workspaces_root / platform::Utf8ToPath(workspace_key);
    const std::filesystem::path session_dir =
        workspace_dir / "sessions" / platform::Utf8ToPath(manifest.session_id);
    std::error_code ec;
    if (std::filesystem::exists(session_dir, ec)) {
        return std::unexpected("session 目录已存在,绝不复用 session_id: " +
                               platform::PathToUtf8(session_dir));
    }
    // §3.1 全目录树。
    const std::filesystem::path session_root = session_dir;
    for (const std::filesystem::path& sub : {
             session_root / "subagents",
             session_root / "checkpoints",
             session_root / "artifacts" / "sha256",
             session_root / "indexes",
             session_root / "derived" / "records",
             session_root / "exports" / "training-v1",
             session_root / "goals",
             session_root / "loops",
             session_root / "workflows",
         }) {
        std::filesystem::create_directories(sub, ec);
        if (ec) {
            return std::unexpected("session 目录建不起: " + platform::PathToUtf8(sub) + ": " +
                                   ec.message());
        }
    }
    // expected<void, E> 成功时 operator bool 为真:按 has_value 反着判,
    // 不能拿 if(result) 当失败分支。
    if (const auto written = WriteSessionJsonAtomic(session_dir, manifest); !written.has_value()) {
        return std::unexpected(written.error());
    }
    TrajectoryDirectory directory;
    directory.workspace_dir_ = workspace_dir;
    directory.session_dir_ = session_dir;
    return directory;
}

std::expected<std::filesystem::path, std::string> TrajectoryDirectory::ReserveMainStream() const {
    if (session_dir_.empty()) {
        return std::unexpected("session 未开");
    }
    return main_stream_path();
}

std::expected<std::filesystem::path, std::string> TrajectoryDirectory::ReserveSubagentStream(
    const std::string& agent_run_id) const {
    if (!IsValidSingleSegment(agent_run_id)) {
        return std::unexpected("agent_run_id 不是合法单段名");
    }
    return session_dir_ / "subagents" / platform::Utf8ToPath(agent_run_id + ".jsonl");
}

std::expected<std::filesystem::path, std::string> TrajectoryDirectory::ReserveWorkflowRun(
    const std::string& workflow_run_id) const {
    if (!IsValidSingleSegment(workflow_run_id)) {
        return std::unexpected("workflow_run_id 不是合法单段名");
    }
    const std::filesystem::path run_dir =
        session_dir_ / "workflows" / platform::Utf8ToPath(workflow_run_id);
    std::error_code ec;
    std::filesystem::create_directories(run_dir / "checkpoints", ec);
    if (ec) {
        return std::unexpected("workflow run 目录建不起: " + platform::PathToUtf8(run_dir) + ": " +
                               ec.message());
    }
    std::filesystem::create_directories(run_dir / "nodes", ec);
    if (ec) {
        return std::unexpected("workflow nodes 目录建不起: " + platform::PathToUtf8(run_dir / "nodes") +
                               ": " + ec.message());
    }
    return run_dir / "workflow.jsonl";
}

std::expected<std::filesystem::path, std::string> TrajectoryDirectory::ReserveWorkflowNodeStream(
    const std::string& workflow_run_id, const std::string& node_run_id) const {
    if (!IsValidSingleSegment(workflow_run_id) || !IsValidSingleSegment(node_run_id)) {
        return std::unexpected("workflow_run_id/node_run_id 不是合法单段名");
    }
    const std::filesystem::path run_dir =
        session_dir_ / "workflows" / platform::Utf8ToPath(workflow_run_id);
    std::error_code ec;
    std::filesystem::create_directories(run_dir / "nodes", ec);
    if (ec) {
        return std::unexpected("workflow nodes 目录建不起: " +
                               platform::PathToUtf8(run_dir / "nodes") + ": " + ec.message());
    }
    return run_dir / "nodes" / platform::Utf8ToPath(node_run_id + ".jsonl");
}

}  // namespace lubancode::trajectory
