#include "trajectory/directory.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1)
#include "platform/paths.hpp"
#include "workspace/manifest.hpp"

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
    // 统一原子写(审计 P1):替掉本文件自备的固定 .tmp 协议。
    return platform::AtomicWriteFile(path, content).has_value();
}

}  // namespace

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
    json["run_kind"] = run_kind;
    json["start_reason"] = start_reason;
    json["previous_session_id"] = previous_session_id.has_value() ? nlohmann::json(*previous_session_id)
                                                                  : nlohmann::json(nullptr);
    json["status"] = status;
    json["created_at_ms"] = created_at_ms;
    json["lubancode_version"] = lubancode_version;
    if (approval_mode.has_value()) {
        json["approval_mode"] = ApprovalModeMachineName(*approval_mode);
    }
    // event schema major 钉进 manifest(Token 账本单 §6.1.1):v1 老档没这键,
    // 读侧按默认 1 兜。
    json["event_schema_version"] = event_schema_version;
    // 存储 v2 合同 §三:两枚迁移键只在 legacy_import 场落盘,空串不写键
    //(与 schema 的"空=缺省"同一口径)。
    if (!subagent_detail.empty()) {
        json["subagent_detail"] = subagent_detail;
    }
    if (!training_policy.empty()) {
        json["training_policy"] = training_policy;
    }
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
    // P0-2:新根写 v2;v1 是旧根旧档形状,读侧仍认(迁移器输入/P0-5),
    // 消费方(index/doctor)在新根见到 v1 按"旧档搬错家"处置。更大的
    // 版本号按合同整份拒读(schema.unsupported_version 语义),不猜。
    if (manifest.schema_version < 1 || manifest.schema_version > 2) {
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
    read_string("run_kind", &manifest.run_kind);
    read_string("start_reason", &manifest.start_reason);
    read_string("lubancode_version", &manifest.lubancode_version);
    if (json.contains("approval_mode") && json.at("approval_mode").is_string()) {
        manifest.approval_mode = ParseApprovalModeOrDefault(json.at("approval_mode").get<std::string>());
    }
    if (json.contains("previous_session_id") && json.at("previous_session_id").is_string()) {
        manifest.previous_session_id = json.at("previous_session_id").get<std::string>();
    }
    if (json.contains("created_at_ms") && json.at("created_at_ms").is_number_integer()) {
        manifest.created_at_ms = json.at("created_at_ms").get<std::int64_t>();
    }
    if (json.contains("event_schema_version") && json.at("event_schema_version").is_number_integer()) {
        manifest.event_schema_version = json.at("event_schema_version").get<int>();
    }
    read_string("subagent_detail", &manifest.subagent_detail);
    read_string("training_policy", &manifest.training_policy);
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
    const std::filesystem::path& workspaces_root, const workspace::WorkspaceIdentity& identity,
    std::int64_t now_ms) {
    if (!identity.valid()) {
        return std::unexpected("identity.path_invalid: 身份没裁决出 workspace_key");
    }
    const std::string key = identity.workspace_key;
    const std::filesystem::path workspace_dir = workspaces_root / platform::Utf8ToPath(key);
    std::error_code ec;
    for (const char* sub : {"sessions", "lifecycle", "tombstones"}) {
        std::filesystem::create_directories(workspace_dir / sub, ec);
        if (ec) {
            return std::unexpected("workspace 目录建不起: " + platform::PathToUtf8(workspace_dir / sub) +
                                   ": " + ec.message());
        }
    }
    // P0-1:manifest 换 v2 合同(workspace::manifest 管读写)。首仓原子写;
    // 已存在则 key 对账 + last_opened/checkout 登记,创建时间以旧账为准。
    if (const auto registered = workspace::OpenOrRegisterWorkspace(workspaces_root, identity, now_ms);
        !registered.has_value()) {
        return std::unexpected(registered.error());
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

TrajectoryDirectory TrajectoryDirectory::OpenExisting(const std::filesystem::path& session_dir) {
    TrajectoryDirectory directory;
    directory.session_dir_ = session_dir;
    directory.workspace_dir_ = session_dir.parent_path().parent_path();
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
