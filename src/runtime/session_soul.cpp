// Soul 会话冻结单 P0:SessionSoulSnapshot 的实现(纯函数 + 会话目录的
// 快照 blob 读写)。合同见 session_soul.hpp 头注。
#include "runtime/session_soul.hpp"

#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include "agent/prompts.hpp"  // StripPromptComments(剥注释的同一把刀,engine 层)
#include "hooks/hash.hpp"     // Sha256Hex
#include "platform/atomic_write.hpp"  // AtomicWriteFile:快照 blob 的唯一原子写口
#include "tools/path_utils.hpp"  // PathToUtf8

namespace lubancode::runtime {

std::string NormalizeSoulContent(const std::string& content) {
    // 注释剥除复用 agent::StripPromptComments(同一把刀:<!-- ... --> 全剥、
    // 两端空白去净)。行尾统一成 \n:Windows 记事本存出 \r\n 与工具存出 \n
    // 不该记成两份魂。
    std::string normalized = agent::StripPromptComments(content);
    std::string out;
    out.reserve(normalized.size());
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        if (normalized[i] == '\r') {
            if (i + 1 < normalized.size() && normalized[i + 1] == '\n') {
                continue;  // \r\n -> \n(下面那位 \n 自己进)
            }
            continue;  // 裸 \r 也折成 \n 之外的丢弃(罕见,统一即可)
        }
        out += normalized[i];
    }
    return out;
}

std::string SessionSoulContentHash(const std::string& content) {
    return hooks::Sha256Hex(NormalizeSoulContent(content));
}

bool UpdateSessionSoulDraft(SessionSoulSnapshot& snapshot, std::string name, std::string content,
                            std::string source) {
    const std::string new_hash = SessionSoulContentHash(content);
    if (new_hash == snapshot.content_hash && name == snapshot.name) {
        return false;  // 内容规范化后相同:不记虚假 pending/revision(§5.2)
    }
    snapshot.name = std::move(name);
    snapshot.content = std::move(content);
    snapshot.source = std::move(source);
    snapshot.content_hash = std::move(new_hash);
    ++snapshot.revision;
    return true;
}

nlohmann::json SessionSoulSnapshotToJson(const SessionSoulSnapshot& snapshot) {
    return nlohmann::json{
        {"schema", "soul-snapshot-v1"},
        {"name", snapshot.name},
        {"content", snapshot.content},
        {"source", snapshot.source},
        {"contentHash", snapshot.content_hash},
        {"revision", snapshot.revision},
        {"locked", snapshot.locked},
    };
}

std::expected<SessionSoulSnapshot, std::string> SessionSoulSnapshotFromJson(const nlohmann::json& json) {
    // nlohmann 单列禁令:const json 上 operator[] 查缺键是 UB,一律先
    // contains() 再取值;类型再核一遍,不半造快照。
    if (!json.is_object()) {
        return std::unexpected("soul 快照不是 JSON 对象");
    }
    if (!json.contains("schema") || !json.at("schema").is_string() ||
        json.at("schema").get<std::string>() != "soul-snapshot-v1") {
        return std::unexpected("soul 快照 schema 不认识");
    }
    const char* required_strings[] = {"name", "content", "source", "contentHash"};
    for (const char* key : required_strings) {
        if (!json.contains(key) || !json.at(key).is_string()) {
            return std::unexpected(std::string("soul 快照缺字段或类型不对: ") + key);
        }
    }
    if (!json.contains("revision") || !json.at("revision").is_number_integer() ||
        !json.contains("locked") || !json.at("locked").is_boolean()) {
        return std::unexpected("soul 快照缺 revision/locked 或类型不对");
    }
    SessionSoulSnapshot snapshot;
    snapshot.name = json.at("name").get<std::string>();
    snapshot.content = json.at("content").get<std::string>();
    snapshot.source = json.at("source").get<std::string>();
    snapshot.content_hash = json.at("contentHash").get<std::string>();
    snapshot.revision = json.at("revision").get<std::int64_t>();
    snapshot.locked = json.at("locked").get<bool>();
    // 材料一致性:正文与自记的 hash 对不上算材料坏,resume 侧据此报错。
    if (snapshot.content_hash != SessionSoulContentHash(snapshot.content)) {
        return std::unexpected("soul 快照正文与 contentHash 对不上(材料损坏)");
    }
    return snapshot;
}

std::string SessionSoulSnapshotFileName() {
    return "soul-snapshot.json";
}

std::expected<void, std::string> WriteSessionSoulSnapshot(const std::filesystem::path& session_dir,
                                                          const SessionSoulSnapshot& snapshot) {
    // 平台统一原子写(见 platform/atomic_write.hpp 的合同):换名原子可见,
    // 失败目标保持原样。快照是恢复材料,ProcessCrashDurability 升一档
    //——锁定那一刻写下的快照,崩溃后也要读得回。
    const auto written = platform::AtomicWriteFile(session_dir / SessionSoulSnapshotFileName(),
                                                   SessionSoulSnapshotToJson(snapshot).dump(2),
                                                   platform::WriteDurability::ProcessCrashDurability);
    if (!written.has_value()) {
        return std::unexpected(written.error().code + ": " + written.error().message);
    }
    return {};
}

std::expected<std::optional<SessionSoulSnapshot>, std::string> ReadSessionSoulSnapshot(
    const std::filesystem::path& session_dir) {
    const std::filesystem::path file = session_dir / SessionSoulSnapshotFileName();
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return std::nullopt;  // 源会话从未锁定过魂:不是错误
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected("soul 快照打不开: " + tools::PathToUtf8(file));
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    nlohmann::json json = nlohmann::json::parse(buffer.str(), nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
        return std::unexpected("soul 快照不是合法 JSON: " + tools::PathToUtf8(file));
    }
    return SessionSoulSnapshotFromJson(json);
}

}  // namespace lubancode::runtime
