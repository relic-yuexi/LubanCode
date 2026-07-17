#include "agent/session_store.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace lubancode::agent {

namespace {

// UTF-8 字符串路径 -> std::filesystem::path(Windows 下要走 u8string 那条路,
// 不然中文路径按 ANSI 代码页解错)。
std::filesystem::path Utf8Path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 一个 UTF-8 首字节起,这个码点占几个字节。非法首字节按 1 处理(总能前进,
// 不会死循环)。
std::size_t Utf8CharLen(unsigned char lead) {
    if ((lead & 0x80U) == 0x00U) return 1;
    if ((lead & 0xE0U) == 0xC0U) return 2;
    if ((lead & 0xF0U) == 0xE0U) return 3;
    if ((lead & 0xF8U) == 0xF0U) return 4;
    return 1;
}

nlohmann::json BlockToJson(const api::ContentBlock& block) {
    nlohmann::json j;
    std::visit(
        [&j](const auto& b) {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, api::TextBlock>) {
                j["type"] = "text";
                j["text"] = b.text;
            } else if constexpr (std::is_same_v<T, api::ToolUseBlock>) {
                j["type"] = "tool_use";
                j["id"] = b.id;
                j["name"] = b.name;
                j["input"] = b.input;
            } else if constexpr (std::is_same_v<T, api::ToolResultBlock>) {
                j["type"] = "tool_result";
                j["tool_use_id"] = b.tool_use_id;
                j["content"] = b.content;
                j["is_error"] = b.is_error;
            }
        },
        block);
    return j;
}

std::optional<api::ContentBlock> BlockFromJson(const nlohmann::json& j) {
    if (!j.is_object()) {
        return std::nullopt;
    }
    const std::string type = j.value("type", std::string());
    if (type == "text") {
        api::TextBlock b;
        b.text = j.value("text", std::string());
        return api::ContentBlock{std::move(b)};
    }
    if (type == "tool_use") {
        api::ToolUseBlock b;
        b.id = j.value("id", std::string());
        b.name = j.value("name", std::string());
        b.input = j.contains("input") ? j["input"] : nlohmann::json::object();
        return api::ContentBlock{std::move(b)};
    }
    if (type == "tool_result") {
        api::ToolResultBlock b;
        b.tool_use_id = j.value("tool_use_id", std::string());
        b.content = j.value("content", std::string());
        b.is_error = j.value("is_error", false);
        return api::ContentBlock{std::move(b)};
    }
    return std::nullopt;  // 认不得的块类型
}

std::string FormatLocalTime(const char* fmt) {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), fmt, &tm_buf);
    return std::string(buf);
}

// Markdown 导出用:一段文本取前 n 行,超了标注省略了多少行。
std::string FirstLinesWithNote(const std::string& text, int max_lines) {
    std::vector<std::string> lines;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }
    if (lines.empty() && !text.empty()) {
        lines.push_back(text);
    }
    std::string out;
    const int total = static_cast<int>(lines.size());
    for (int i = 0; i < total && i < max_lines; ++i) {
        out += lines[static_cast<std::size_t>(i)];
        out += "\n";
    }
    if (total > max_lines) {
        out += "...(共 " + std::to_string(total) + " 行,已省略其余)\n";
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// meta
// ---------------------------------------------------------------------------

std::string SerializeSessionMeta(const SessionMeta& meta) {
    nlohmann::json j;
    j["version"] = meta.version;
    j["wire"] = meta.wire;
    j["model"] = meta.model;
    j["cwd"] = meta.cwd;
    j["started_at"] = meta.started_at;
    return j.dump();
}

std::optional<SessionMeta> ParseSessionMeta(const std::string& line) {
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || !j.contains("version") || !j["version"].is_number_integer()) {
        return std::nullopt;
    }
    SessionMeta meta;
    meta.version = j["version"].get<int>();
    meta.wire = j.value("wire", std::string());
    meta.model = j.value("model", std::string());
    meta.cwd = j.value("cwd", std::string());
    meta.started_at = j.value("started_at", std::string());
    return meta;
}

// ---------------------------------------------------------------------------
// 消息
// ---------------------------------------------------------------------------

std::string SerializeSessionMessage(const api::Message& message, const std::string& ts) {
    nlohmann::json j;
    j["role"] = message.role == api::Role::Assistant ? "assistant" : "user";
    nlohmann::json blocks = nlohmann::json::array();
    for (const auto& block : message.content) {
        blocks.push_back(BlockToJson(block));
    }
    j["content"] = std::move(blocks);
    j["ts"] = ts;
    return j.dump();
}

std::optional<api::Message> DeserializeSessionMessage(const std::string& line) {
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || !j.contains("role") || !j.contains("content") || !j["content"].is_array()) {
        return std::nullopt;
    }
    const std::string role = j.value("role", std::string());
    if (role != "user" && role != "assistant") {
        return std::nullopt;
    }
    api::Message message;
    message.role = role == "assistant" ? api::Role::Assistant : api::Role::User;
    for (const auto& bj : j["content"]) {
        auto block = BlockFromJson(bj);
        if (block.has_value()) {
            message.content.push_back(std::move(*block));
        }
        // 认不得的块跳过,别把整条消息废掉
    }
    return message;
}

// ---------------------------------------------------------------------------
// 会话 id
// ---------------------------------------------------------------------------

std::string MakeSessionSlug(const std::string& first_user_text, std::size_t max_chars) {
    std::string out;
    std::size_t chars = 0;
    std::size_t pos = 0;
    bool last_dash = true;  // 起手视为刚放过 '-',天然剥掉开头的 '-'
    while (pos < first_user_text.size() && chars < max_chars) {
        const auto lead = static_cast<unsigned char>(first_user_text[pos]);
        std::size_t len = Utf8CharLen(lead);
        if (pos + len > first_user_text.size()) {
            len = 1;  // 尾巴上残缺的多字节序列,按单字节危险字符处理
        }
        ++chars;
        if (len == 1) {
            const char c = first_user_text[pos];
            const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                              c == '.' || c == '_' || c == '-';
            // '.' 单独再防一手:不让 slug 以点收尾(Windows 文件名忌讳),
            // 收尾清理在函数末尾统一做,这里只分安全/危险两类。
            if (safe) {
                out += c;
                last_dash = false;
            } else if (!last_dash) {
                out += '-';
                last_dash = true;
            }
        } else {
            out.append(first_user_text, pos, len);  // 中文等多字节字符原样留
            last_dash = false;
        }
        pos += len;
    }
    while (!out.empty() && (out.back() == '-' || out.back() == '.')) {
        out.pop_back();
    }
    if (out.empty()) {
        return "untitled";
    }
    return out;
}

std::string MakeSessionId(const std::string& timestamp, const std::string& first_user_text) {
    return timestamp + "-" + MakeSessionSlug(first_user_text);
}

std::string TruncateUtf8Chars(const std::string& text, std::size_t max_chars) {
    std::size_t chars = 0;
    std::size_t pos = 0;
    while (pos < text.size() && chars < max_chars) {
        std::size_t len = Utf8CharLen(static_cast<unsigned char>(text[pos]));
        if (pos + len > text.size()) {
            len = 1;
        }
        pos += len;
        ++chars;
    }
    if (pos >= text.size()) {
        return text;
    }
    return text.substr(0, pos) + "…";
}

// ---------------------------------------------------------------------------
// 成对修补
// ---------------------------------------------------------------------------

int RepairToolPairs(std::vector<api::Message>& history) {
    int repaired = 0;

    // 第一遍:收集全部 tool_use id / tool_result id。
    std::set<std::string> tool_use_ids;
    for (const auto& message : history) {
        for (const auto& block : message.content) {
            if (const auto* use = std::get_if<api::ToolUseBlock>(&block)) {
                tool_use_ids.insert(use->id);
            }
        }
    }

    // 第二遍:孤儿 tool_result(对不上任何 tool_use)删掉——重放时 API 同样
    // 会拒。顺手把删空了的 user 消息整条去掉。
    for (auto it = history.begin(); it != history.end();) {
        auto& blocks = it->content;
        std::erase_if(blocks, [&](const api::ContentBlock& block) {
            const auto* result = std::get_if<api::ToolResultBlock>(&block);
            return result != nullptr && tool_use_ids.count(result->tool_use_id) == 0;
        });
        if (blocks.empty()) {
            it = history.erase(it);
        } else {
            ++it;
        }
    }

    // 第三遍:每条 assistant 消息里的 tool_use,紧跟着的 user 消息里必须有
    // 对应 tool_result;缺的补进去(下一条是 user 就补进它,不是就插一条新
    // user 消息)。
    for (std::size_t i = 0; i < history.size(); ++i) {
        if (history[i].role != api::Role::Assistant) {
            continue;
        }
        std::vector<std::string> pending;
        for (const auto& block : history[i].content) {
            if (const auto* use = std::get_if<api::ToolUseBlock>(&block)) {
                pending.push_back(use->id);
            }
        }
        if (pending.empty()) {
            continue;
        }
        const bool next_is_user = i + 1 < history.size() && history[i + 1].role == api::Role::User;
        std::set<std::string> answered;
        if (next_is_user) {
            for (const auto& block : history[i + 1].content) {
                if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                    answered.insert(result->tool_use_id);
                }
            }
        }
        std::vector<api::ContentBlock> patches;
        for (const auto& id : pending) {
            if (answered.count(id) != 0) {
                continue;
            }
            api::ToolResultBlock patch;
            patch.tool_use_id = id;
            patch.content = "[会话恢复:结果缺失]";
            patch.is_error = true;
            patches.emplace_back(std::move(patch));
            ++repaired;
        }
        if (patches.empty()) {
            continue;
        }
        if (next_is_user) {
            // 补在该 user 消息开头——tool_result 块得排在普通文本前头才最像
            // 正常轮次的形状(anthropic 要求 tool_result 紧跟 tool_use)。
            auto& target = history[i + 1].content;
            target.insert(target.begin(), patches.begin(), patches.end());
        } else {
            api::Message filler;
            filler.role = api::Role::User;
            filler.content = std::move(patches);
            history.insert(history.begin() + static_cast<std::ptrdiff_t>(i) + 1, std::move(filler));
        }
    }
    return repaired;
}

// ---------------------------------------------------------------------------
// 整文件解析 / 导出
// ---------------------------------------------------------------------------

std::optional<LoadedSession> ParseSessionFile(const std::string& content) {
    std::istringstream iss(content);
    std::string line;
    if (!std::getline(iss, line)) {
        return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    const auto meta = ParseSessionMeta(line);
    if (!meta.has_value()) {
        return std::nullopt;
    }
    LoadedSession session;
    session.meta = *meta;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        auto message = DeserializeSessionMessage(line);
        if (message.has_value()) {
            session.messages.push_back(std::move(*message));
        } else {
            session.skipped_lines += 1;
        }
    }
    session.repaired = RepairToolPairs(session.messages);
    return session;
}

std::string ExportSessionMarkdown(const SessionMeta& meta, const std::vector<api::Message>& messages,
                                   const std::string& session_id, int max_result_lines) {
    // tool_use id -> 结果内容,就近配对(id 冲突在正常存档里不会有,真有也
    // 只是后写的盖前头的,导出展示无伤大雅)。
    struct ResultRef {
        std::string content;
        bool is_error = false;
    };
    std::map<std::string, ResultRef> results;
    for (const auto& message : messages) {
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                results[result->tool_use_id] = ResultRef{result->content, result->is_error};
            }
        }
    }

    std::string out;
    out += "# 会话 " + session_id + "\n\n";
    out += "- 开始时间: " + (meta.started_at.empty() ? std::string("(未知)") : meta.started_at) + "\n";
    out += "- wire: " + meta.wire + "\n";
    out += "- model: " + meta.model + "\n";
    out += "- cwd: " + meta.cwd + "\n";

    for (const auto& message : messages) {
        // 这条消息里有没有值得单开一节的正文(文本块)?只装着 tool_result
        // 的 user 消息不开"用户"节——结果已折进对应 tool_use 的 details。
        std::string text;
        for (const auto& block : message.content) {
            if (const auto* tb = std::get_if<api::TextBlock>(&block)) {
                if (!text.empty()) {
                    text += "\n";
                }
                text += tb->text;
            }
        }
        const bool assistant = message.role == api::Role::Assistant;
        bool has_tool_use = false;
        for (const auto& block : message.content) {
            if (std::holds_alternative<api::ToolUseBlock>(block)) {
                has_tool_use = true;
                break;
            }
        }
        if (text.empty() && !has_tool_use) {
            continue;
        }
        out += assistant ? "\n## 助手\n\n" : "\n## 用户\n\n";
        if (!text.empty()) {
            out += text;
            out += "\n";
        }
        for (const auto& block : message.content) {
            const auto* use = std::get_if<api::ToolUseBlock>(&block);
            if (use == nullptr) {
                continue;
            }
            out += "\n<details>\n<summary>工具调用: " + use->name + "</summary>\n\n";
            out += "输入:\n\n```json\n" + use->input.dump(2) + "\n```\n";
            const auto found = results.find(use->id);
            if (found != results.end()) {
                out += "\n结果" + std::string(found->second.is_error ? "(出错)" : "") + ":\n\n```\n";
                out += FirstLinesWithNote(found->second.content, max_result_lines);
                out += "```\n";
            } else {
                out += "\n结果: (缺失)\n";
            }
            out += "\n</details>\n";
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 磁盘薄壳
// ---------------------------------------------------------------------------

SessionStore::SessionStore(std::string sessions_dir) : sessions_dir_(std::move(sessions_dir)) {}

bool SessionStore::Begin(const SessionMeta& meta, const std::string& session_id) {
    Reset();
    std::error_code ec;
    std::filesystem::create_directories(Utf8Path(sessions_dir_), ec);
    if (ec) {
        return false;
    }
    // 撞名(同秒 /clear 又发同样开头的话)就加 -2、-3……不盖旧档。
    std::string id = session_id;
    std::filesystem::path path = Utf8Path(sessions_dir_) / Utf8Path(id + ".jsonl");
    for (int i = 2; std::filesystem::exists(path, ec) && i < 100; ++i) {
        id = session_id + "-" + std::to_string(i);
        path = Utf8Path(sessions_dir_) / Utf8Path(id + ".jsonl");
    }
    out_.open(path, std::ios::binary | std::ios::app);
    if (!out_.is_open()) {
        return false;
    }
    session_id_ = id;
    file_path_ = PathToUtf8(path);
    out_ << SerializeSessionMeta(meta) << "\n";
    out_.flush();
    return out_.good();
}

bool SessionStore::ResumeAt(const std::string& file_path, const std::string& session_id) {
    Reset();
    out_.open(Utf8Path(file_path), std::ios::binary | std::ios::app);
    if (!out_.is_open()) {
        return false;
    }
    session_id_ = session_id;
    file_path_ = file_path;
    return true;
}

bool SessionStore::AppendMessage(const api::Message& message) {
    if (!out_.is_open()) {
        return false;
    }
    out_ << SerializeSessionMessage(message, NowTimestamp()) << "\n";
    out_.flush();
    return out_.good();
}

void SessionStore::Reset() {
    if (out_.is_open()) {
        out_.close();
    }
    session_id_.clear();
    file_path_.clear();
}

std::vector<SessionListEntry> ListSessions(const std::string& sessions_dir, std::size_t limit) {
    namespace fs = std::filesystem;
    std::vector<SessionListEntry> entries;
    std::error_code ec;
    fs::directory_iterator it(Utf8Path(sessions_dir), ec);
    if (ec) {
        return entries;  // 目录不存在:还没存过任何会话
    }
    for (const auto& dir_entry : it) {
        if (!dir_entry.is_regular_file(ec) || dir_entry.path().extension() != ".jsonl") {
            continue;
        }
        SessionListEntry entry;
        entry.id = PathToUtf8(dir_entry.path().stem());
        entry.file_path = PathToUtf8(dir_entry.path());
        entries.push_back(std::move(entry));
    }
    // id 以 yyyymmdd-HHMMSS 起头,字典倒序即时间倒序。
    std::sort(entries.begin(), entries.end(),
              [](const SessionListEntry& a, const SessionListEntry& b) { return a.id > b.id; });
    if (entries.size() > limit) {
        entries.resize(limit);
    }
    // 只给留下来的这最多 limit 场读文件补详情(别把整目录都读一遍)。
    for (auto& entry : entries) {
        const auto content = ReadSessionFileBytes(entry.file_path);
        if (!content.has_value()) {
            continue;
        }
        std::istringstream iss(*content);
        std::string line;
        bool first = true;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            if (first) {
                first = false;
                if (const auto meta = ParseSessionMeta(line); meta.has_value()) {
                    entry.started_at = meta->started_at;
                    continue;  // meta 行不算消息
                }
                // 首行不是 meta:旧格式/坏文件,照样往下数
            }
            entry.message_count += 1;
            if (entry.first_user_text.empty()) {
                const auto message = DeserializeSessionMessage(line);
                if (message.has_value() && message->role == api::Role::User) {
                    for (const auto& block : message->content) {
                        if (const auto* tb = std::get_if<api::TextBlock>(&block); tb != nullptr && !tb->text.empty()) {
                            entry.first_user_text = tb->text.substr(0, tb->text.find('\n'));
                            break;
                        }
                    }
                }
            }
        }
    }
    return entries;
}

std::optional<std::string> ReadSessionFileBytes(const std::string& file_path) {
    std::ifstream file(Utf8Path(file_path), std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream oss;
    oss << file.rdbuf();
    return oss.str();
}

std::string NowTimestamp() { return FormatLocalTime("%Y-%m-%d %H:%M:%S"); }

std::string NowIdTimestamp() { return FormatLocalTime("%Y%m%d-%H%M%S"); }

}  // namespace lubancode::agent
