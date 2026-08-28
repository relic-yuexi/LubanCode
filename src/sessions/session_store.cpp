#include "sessions/session_store.hpp"

#include <algorithm>
#include <iterator>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "platform/json_safe.hpp"  // DumpJsonSanitized:落盘行的编码窄边界
#include "platform/text_encoding.hpp"  // SanitizeExternalText:旧会话档读入时的编码关口
#include "platform/wall_clock.hpp"  // 统一墙钟(批五):ts 的钟同源五套台账

namespace lubancode::sessions {

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

// 递归把 JSON 树里所有字符串字段过一遍 SanitizeExternalText(合法时零
// 成本)。工具入参这类任意 JSON 从旧档读入时用它洗,免得坏串漏到 wire。
// 返回 false 表示这棵树连结构都坏了(根不是 object/array,或清洗后仍
// 无法作为工具入参),调用方给个空对象兜底。
bool SanitizeJsonTree(nlohmann::json& value) {
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        if (!platform::IsValidUtf8(text)) {
            value = platform::SanitizeExternalText(text);
        }
        return true;
    }
    if (value.is_array()) {
        for (auto& item : value) {
            SanitizeJsonTree(item);
        }
        return true;
    }
    if (value.is_object()) {
        for (auto& item : value) {
            SanitizeJsonTree(item);
        }
        return true;
    }
    return false;
}

nlohmann::json BlockToJson(const api::ContentBlock& block) {
    nlohmann::json j;
    std::visit(
        [&j](const auto& b) {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, api::TextBlock>) {
                j["type"] = "text";
                j["text"] = b.text;
            } else if constexpr (std::is_same_v<T, api::ImageBlock>) {
                j["type"] = "image";
                j["media_type"] = b.media_type;
                j["data"] = b.data;
                j["filename"] = b.filename;
                j["width"] = b.width;
                j["height"] = b.height;
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
                // MCP 富结果单 P0.3:富块与 structuredContent 入档;块序即
                // 真序,图片/音频块只存引用(字节在会话 mcp-artifacts/ 目录)。
                if (!b.blocks.empty()) {
                    nlohmann::json blocks = nlohmann::json::array();
                    for (const auto& rich : b.blocks) {
                        blocks.push_back(tools::BlockToJson(rich));
                    }
                    j["blocks"] = std::move(blocks);
                }
                if (b.structured_content.has_value()) {
                    j["structured_content"] = *b.structured_content;
                }
            } else if constexpr (std::is_same_v<T, api::ThinkingBlock>) {
                j["type"] = "thinking";
                j["text"] = b.text;
                j["signature"] = b.signature;
            } else if constexpr (std::is_same_v<T, api::ModelImageBlock>) {
                // 模型输出图片的引用块:只有路径/尺寸/sha,没有 base64——
                // 正文落在会话 images/ 目录里,JSONL 只记引用(ccmoon 巡检
                // 单 P0:session/export/resume 留引用,不塞正文)。
                j["type"] = "model_image";
                j["id"] = b.id;
                j["filename"] = b.filename;
                j["path"] = b.path;
                j["mime_type"] = b.mime_type;
                j["width"] = b.width;
                j["height"] = b.height;
                j["bytes"] = b.bytes;
                j["sha256"] = b.sha256;
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
        b.text = platform::SanitizeExternalText(j.value("text", std::string()));
        return api::ContentBlock{std::move(b)};
    }
    if (type == "image") {
        api::ImageBlock b;
        b.media_type = j.value("media_type", std::string());
        b.data = j.value("data", std::string());
        b.filename = j.value("filename", std::string());
        b.width = j.value("width", 0U);
        b.height = j.value("height", 0U);
        if (b.media_type.empty() || b.data.empty()) {
            return std::nullopt;
        }
        return api::ContentBlock{std::move(b)};
    }
    if (type == "tool_use") {
        api::ToolUseBlock b;
        b.id = platform::SanitizeExternalText(j.value("id", std::string()));
        b.name = platform::SanitizeExternalText(j.value("name", std::string()));
        b.input = j.contains("input") ? j["input"] : nlohmann::json::object();
        // 工具入参是任意 JSON,递归洗掉里面的坏串(旧档/崩溃截断行
        // 带进来的),免得漏到 wire 上触发 316 兜底。
        if (!SanitizeJsonTree(b.input)) {
            b.input = nlohmann::json::object();
        }
        return api::ContentBlock{std::move(b)};
    }
    if (type == "tool_result") {
        api::ToolResultBlock b;
        b.tool_use_id = platform::SanitizeExternalText(j.value("tool_use_id", std::string()));
        b.content = platform::SanitizeExternalText(j.value("content", std::string()));
        b.is_error = j.value("is_error", false);
        // MCP 富结果单:恢复时块与 structuredContent 照读;旧档(只有
        // content 字符串)blocks 为空,走纯文本路,行为与从前一字不差。
        // 认不出的块类型弃那只块,不弃整条结果——工具调用配对不能丢。
        if (j.contains("blocks") && j["blocks"].is_array()) {
            for (const auto& rich : j["blocks"]) {
                if (auto block = tools::BlockFromJson(rich)) {
                    b.blocks.push_back(std::move(*block));
                }
            }
        }
        if (j.contains("structured_content") && j["structured_content"].is_object()) {
            b.structured_content = j["structured_content"];
            tools::SanitizeJsonTextInPlace(*b.structured_content);
        }
        return api::ContentBlock{std::move(b)};
    }
    if (type == "thinking") {
        api::ThinkingBlock b;
        b.text = platform::SanitizeExternalText(j.value("text", std::string()));
        b.signature = platform::SanitizeExternalText(j.value("signature", std::string()));
        return api::ContentBlock{std::move(b)};
    }
    if (type == "model_image") {
        api::ModelImageBlock b;
        b.id = platform::SanitizeExternalText(j.value("id", std::string()));
        b.filename = platform::SanitizeExternalText(j.value("filename", std::string()));
        b.path = platform::SanitizeExternalText(j.value("path", std::string()));
        b.mime_type = platform::SanitizeExternalText(j.value("mime_type", std::string()));
        b.width = j.value("width", 0U);
        b.height = j.value("height", 0U);
        if (j.contains("bytes") && j["bytes"].is_number_unsigned()) {
            b.bytes = j["bytes"].get<std::size_t>();
        }
        b.sha256 = platform::SanitizeExternalText(j.value("sha256", std::string()));
        if (b.filename.empty() || b.path.empty()) {
            return std::nullopt;  // 引用块没了落点就是坏块
        }
        return api::ContentBlock{std::move(b)};
    }
    return std::nullopt;  // 认不得的块类型
}

// 消息 -> JSON 对象(role + content 块数组)。消息行和 compact 事件里的
// archive 共用这一份,格式天然一致。
nlohmann::json MessageToJson(const api::Message& message) {
    nlohmann::json j;
    j["role"] = message.role == api::Role::Assistant ? "assistant" : "user";
    nlohmann::json blocks = nlohmann::json::array();
    for (const auto& block : message.content) {
        blocks.push_back(BlockToJson(block));
    }
    j["content"] = std::move(blocks);
    return j;
}

// JSON 对象 -> 消息。缺 role/content、角色认不得,给 nullopt;认不得的
// 内容块跳过,不废整条。
std::optional<api::Message> MessageFromJson(const nlohmann::json& j) {
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
    }
    return message;
}

std::string FormatLocalTime(const char* fmt) {
    // 批五:钟读 platform 统一墙钟(五套台账同源);本地串格式不动。
    const std::time_t now = platform::WallClockToTimeT(platform::WallClockNowMs());
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
    nlohmann::json j = MessageToJson(message);
    j["ts"] = ts;
    // 窄边界兜底:万一有坏串漏到这里(工具结果本该在 RunOneTool 就规范
    // 化过),清洗后落盘,保证每一行都能重新解析,/resume 不吃半行。
    return platform::DumpJsonSanitized(j);
}

std::optional<api::Message> DeserializeSessionMessage(const std::string& line) {
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    return MessageFromJson(j);
}

// ---------------------------------------------------------------------------
// 事件行(compact / title)
// ---------------------------------------------------------------------------

namespace {
// kept_ids 字段(跳跃保留集)的读写:老档没有,缺省空表 = 连续保留老语义。
void WriteKeptIndices(nlohmann::json& j, const std::vector<std::size_t>& kept_indices) {
    if (kept_indices.empty()) {
        return;  // 连续保留:不写字段,字节与老档一致
    }
    nlohmann::json ids = nlohmann::json::array();
    for (const std::size_t index : kept_indices) {
        ids.push_back(index);
    }
    j["kept_ids"] = std::move(ids);
}
std::vector<std::size_t> ReadKeptIndices(const nlohmann::json& j) {
    std::vector<std::size_t> indices;
    if (!j.contains("kept_ids") || !j["kept_ids"].is_array()) {
        return indices;
    }
    for (const auto& item : j["kept_ids"]) {
        if (item.is_number_unsigned()) {
            indices.push_back(item.get<std::size_t>());
        }
    }
    return indices;
}
}  // namespace

std::string SerializeCompactEvent(const CompactEvent& event, const std::string& ts) {
    nlohmann::json j;
    j["type"] = "compact";
    j["archive"] = MessageToJson(event.archive);
    j["kept_from"] = event.kept_from;
    WriteKeptIndices(j, event.kept_indices);
    j["ts"] = ts;
    return platform::DumpJsonSanitized(j);  // 坏串窄边界,同 SerializeSessionMessage
}

std::optional<CompactEvent> ParseCompactEvent(const std::string& line) {
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || j.value("type", std::string()) != "compact") {
        return std::nullopt;
    }
    if (!j.contains("archive") || !j.contains("kept_from") || !j["kept_from"].is_number_integer()) {
        return std::nullopt;
    }
    const auto kept = j["kept_from"].get<long long>();
    if (kept < 0) {
        return std::nullopt;
    }
    auto archive = MessageFromJson(j["archive"]);
    if (!archive.has_value()) {
        return std::nullopt;
    }
    CompactEvent event;
    event.archive = std::move(*archive);
    event.kept_from = static_cast<std::size_t>(kept);
    event.kept_indices = ReadKeptIndices(j);
    return event;
}

CompactEvent MakeCompactEvent(std::size_t old_history_size, const std::vector<api::Message>& new_history,
                               std::vector<std::size_t> kept_indices) {
    CompactEvent event;
    if (new_history.empty()) {
        // 不该发生(BuildCompactedHistory 至少给一条),纯防御:空 archive、
        // 全不保留。
        event.archive.role = api::Role::User;
        event.kept_from = old_history_size;
        return event;
    }
    event.archive = new_history.front();
    const std::size_t kept_count = new_history.size() - 1;
    event.kept_from = old_history_size >= kept_count ? old_history_size - kept_count : 0;
    event.kept_indices = std::move(kept_indices);
    return event;
}

std::vector<api::Message> ApplyCompactEvent(std::vector<api::Message> effective, const CompactEvent& event) {
    std::vector<api::Message> out;
    if (!event.kept_indices.empty()) {
        // 跳跃保留集(组收法):按下标取,被替换的旧历史不再装回来。首下标
        // 的原消息已被 archive 并进文本(压缩时 new_history[0] = archive +
        // 首条),不再重复取——与连续路 kept_from 的老语义同型。
        out.reserve(event.kept_indices.size());
        out.push_back(event.archive);
        for (std::size_t i = 1; i < event.kept_indices.size(); ++i) {
            const std::size_t index = event.kept_indices[i];
            if (index < effective.size()) {
                out.push_back(std::move(effective[index]));
            }
        }
        return out;
    }
    const std::size_t from = (std::min)(event.kept_from, effective.size());
    out.reserve(1 + (effective.size() - from));
    out.push_back(event.archive);
    for (std::size_t i = from; i < effective.size(); ++i) {
        out.push_back(std::move(effective[i]));
    }
    return out;
}

// ---------------------------------------------------------------------------
// compact_v2 事件
// ---------------------------------------------------------------------------

std::string SerializeCompactV2Event(const CompactV2Event& event, const std::string& ts) {
    nlohmann::json j;
    j["type"] = "compact_v2";
    j["schema"] = 2;
    j["epoch"] = event.epoch;
    j["archive"] = MessageToJson(event.archive);
    j["kept_from"] = event.kept_from;
    WriteKeptIndices(j, event.kept_indices);
    if (!event.manifest.is_null()) {
        j["manifest"] = event.manifest;
    }
    if (!event.metrics.is_null()) {
        j["metrics"] = event.metrics;
    }
    j["ts"] = ts;
    return platform::DumpJsonSanitized(j);  // 坏串窄边界,同 SerializeSessionMessage
}

std::optional<CompactV2Event> ParseCompactV2Event(const std::string& line) {
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || j.value("type", std::string()) != "compact_v2") {
        return std::nullopt;
    }
    if (!j.contains("archive") || !j.contains("kept_from") || !j["kept_from"].is_number_integer()) {
        return std::nullopt;
    }
    const auto kept = j["kept_from"].get<long long>();
    if (kept < 0) {
        return std::nullopt;
    }
    auto archive = MessageFromJson(j["archive"]);
    if (!archive.has_value()) {
        return std::nullopt;
    }
    CompactV2Event event;
    event.archive = std::move(*archive);
    event.kept_from = static_cast<std::size_t>(kept);
    event.kept_indices = ReadKeptIndices(j);
    if (j.contains("epoch") && j["epoch"].is_number_integer()) {
        event.epoch = j["epoch"].get<int>();
    }
    if (j.contains("manifest")) {
        event.manifest = j["manifest"];
    }
    if (j.contains("metrics")) {
        event.metrics = j["metrics"];
    }
    return event;
}

CompactV2Event UpgradeToV2(const CompactEvent& event, int epoch, nlohmann::json manifest, nlohmann::json metrics) {
    CompactV2Event v2;
    v2.archive = event.archive;
    v2.kept_from = event.kept_from;
    v2.kept_indices = event.kept_indices;
    v2.epoch = epoch;
    v2.manifest = std::move(manifest);
    v2.metrics = std::move(metrics);
    return v2;
}

CompactEvent AsCompactEvent(const CompactV2Event& event) {
    CompactEvent v1;
    v1.archive = event.archive;
    v1.kept_from = event.kept_from;
    v1.kept_indices = event.kept_indices;
    return v1;
}

std::string SerializeTitleEvent(const std::string& title, const std::string& ts) {
    nlohmann::json j;
    j["type"] = "title";
    j["title"] = title;
    j["ts"] = ts;
    return platform::DumpJsonSanitized(j);  // 坏串窄边界,同 SerializeSessionMessage
}

std::optional<std::string> ParseTitleEvent(const std::string& line) {
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || j.value("type", std::string()) != "title") {
        return std::nullopt;
    }
    if (!j.contains("title") || !j["title"].is_string()) {
        return std::nullopt;
    }
    return j["title"].get<std::string>();
}

std::string SerializeCwdEvent(const std::string& cwd, const std::string& ts) {
    nlohmann::json j;
    j["type"] = "cwd";
    j["cwd"] = cwd;
    j["ts"] = ts;
    return j.dump();
}

std::optional<std::string> ParseCwdEvent(const std::string& line) {
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || j.value("type", std::string()) != "cwd") {
        return std::nullopt;
    }
    if (!j.contains("cwd") || !j["cwd"].is_string()) {
        return std::nullopt;
    }
    return j["cwd"].get<std::string>();
}

// ---------------------------------------------------------------------------
// queue 事件(排队消息落档)
// ---------------------------------------------------------------------------

std::string SerializeQueueEvent(const std::vector<ArchivedQueueItem>& items, const std::string& ts) {
    nlohmann::json j;
    j["type"] = "queue";
    nlohmann::json array = nlohmann::json::array();
    for (const auto& item : items) {
        nlohmann::json entry;
        entry["id"] = item.id;
        entry["target"] = item.subagent ? ("#" + std::to_string(item.task_id)) : "main";
        entry["text"] = item.text;
        entry["attempts"] = item.attempts;
        array.push_back(std::move(entry));
    }
    j["items"] = std::move(array);
    j["ts"] = ts;
    return platform::DumpJsonSanitized(j);  // 排队正文也是用户键入,坏串窄边界同消息行
}

std::optional<std::vector<ArchivedQueueItem>> ParseQueueEvent(const std::string& line) {
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || j.value("type", std::string()) != "queue") {
        return std::nullopt;
    }
    if (!j.contains("items") || !j["items"].is_array()) {
        return std::nullopt;
    }
    std::vector<ArchivedQueueItem> out;
    for (const auto& entry : j["items"]) {
        if (!entry.is_object() || !entry.contains("text") || !entry["text"].is_string()) {
            continue;  // 坏条目跳过,不废整份(事件行通用取舍)
        }
        ArchivedQueueItem item;
        if (entry.contains("id") && entry["id"].is_number_unsigned()) {
            item.id = entry["id"].get<std::uint64_t>();
        }
        item.text = entry["text"].get<std::string>();
        if (item.text.empty() || item.id == 0) {
            continue;  // 空正文/无 id 不是一条可恢复的排队消息
        }
        if (entry.contains("target") && entry["target"].is_string()) {
            const std::string target = entry["target"].get<std::string>();
            if (!target.empty() && target.front() == '#') {
                item.subagent = true;
                item.task_id = std::atoi(target.c_str() + 1);
                if (item.task_id <= 0) {
                    continue;  // "#abc" 这类认不出的目标号,救不了
                }
            }
        }
        if (entry.contains("attempts") && entry["attempts"].is_number_integer()) {
            item.attempts = entry["attempts"].get<int>();
            if (item.attempts < 0) {
                item.attempts = 0;
            }
        }
        out.push_back(std::move(item));
    }
    return out;
}

// ---------------------------------------------------------------------------
// mode / plan / plan_review 事件(Plan 模式单)
// ---------------------------------------------------------------------------

std::string SerializeModeEvent(const ModeEvent& event, const std::string& ts) {
    nlohmann::json j;
    j["type"] = "mode_v1";
    j["mode"] = event.mode;
    j["reason"] = event.reason;
    j["revision"] = event.revision;
    j["ts"] = ts;
    return platform::DumpJsonSanitized(j);
}

std::optional<ModeEvent> ParseModeEvent(const std::string& line) {
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || j.value("type", std::string()) != "mode_v1") {
        return std::nullopt;
    }
    ModeEvent event;
    if (!j.contains("mode") || !j["mode"].is_string()) {
        return std::nullopt;
    }
    event.mode = j["mode"].get<std::string>();
    if (event.mode != "plan" && event.mode != "default") {
        return std::nullopt;  // 认不得的档位当坏行,不猜
    }
    if (j.contains("reason") && j["reason"].is_string()) {
        event.reason = j["reason"].get<std::string>();
    }
    if (j.contains("revision") && j["revision"].is_number_unsigned()) {
        event.revision = j["revision"].get<std::uint64_t>();
    }
    return event;
}

std::string SerializePlanEvent(const PlanEvent& event, const std::string& ts) {
    nlohmann::json j;
    j["type"] = "plan_v1";
    j["plan_id"] = event.plan_id;
    j["revision"] = event.revision;
    j["state"] = event.state;
    j["sha256"] = event.sha256;
    if (!event.markdown.empty()) {
        j["markdown"] = event.markdown;
    }
    if (!event.artifact_ref.empty()) {
        j["artifact_ref"] = event.artifact_ref;
    }
    if (!event.turn_id.empty()) {
        j["turn_id"] = event.turn_id;
    }
    j["ts"] = ts;
    return platform::DumpJsonSanitized(j);  // 计划正文是模型产出,坏串窄边界同消息行
}

std::optional<PlanEvent> ParsePlanEvent(const std::string& line) {
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || j.value("type", std::string()) != "plan_v1") {
        return std::nullopt;
    }
    PlanEvent event;
    const auto read_str = [&j](const char* key, std::string& out) {
        if (j.contains(key) && j[key].is_string()) {
            out = j[key].get<std::string>();
        }
    };
    read_str("plan_id", event.plan_id);
    read_str("state", event.state);
    read_str("sha256", event.sha256);
    read_str("markdown", event.markdown);
    read_str("artifact_ref", event.artifact_ref);
    read_str("turn_id", event.turn_id);
    if (event.plan_id.empty() || event.sha256.empty()) {
        return std::nullopt;  // 没身份/没锚的计划行救不了,跳行不废场
    }
    if (j.contains("revision") && j["revision"].is_number_unsigned()) {
        event.revision = j["revision"].get<std::uint64_t>();
    }
    return event;
}

std::string SerializePlanReviewEvent(const PlanReviewEvent& event, const std::string& ts) {
    nlohmann::json j;
    j["type"] = "plan_review_v1";
    j["plan_id"] = event.plan_id;
    j["revision"] = event.revision;
    j["decision"] = event.decision;
    if (!event.execution_permission.empty()) {
        j["execution_permission"] = event.execution_permission;
    }
    j["ts"] = ts;
    return platform::DumpJsonSanitized(j);
}

std::optional<PlanReviewEvent> ParsePlanReviewEvent(const std::string& line) {
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || j.value("type", std::string()) != "plan_review_v1") {
        return std::nullopt;
    }
    PlanReviewEvent event;
    const auto read_str = [&j](const char* key, std::string& out) {
        if (j.contains(key) && j[key].is_string()) {
            out = j[key].get<std::string>();
        }
    };
    read_str("plan_id", event.plan_id);
    read_str("decision", event.decision);
    read_str("execution_permission", event.execution_permission);
    if (event.plan_id.empty() ||
        (event.decision != "approved" && event.decision != "rejected" && event.decision != "continued")) {
        return std::nullopt;
    }
    if (j.contains("revision") && j["revision"].is_number_unsigned()) {
        event.revision = j["revision"].get<std::uint64_t>();
    }
    return event;
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

std::string AbbreviateUtf8Middle(const std::string& text, std::size_t max_chars) {
    if (max_chars < 2) {
        return TruncateUtf8Chars(text, max_chars);
    }
    // 先数码点、记每个码点的字节起点。
    std::vector<std::size_t> starts;
    std::size_t pos = 0;
    while (pos < text.size()) {
        starts.push_back(pos);
        std::size_t len = Utf8CharLen(static_cast<unsigned char>(text[pos]));
        if (pos + len > text.size()) {
            len = 1;
        }
        pos += len;
    }
    if (starts.size() <= max_chars) {
        return text;
    }
    // 头尾各留一半(省略号占一个字位),路径场景开头是盘符、结尾是目录名,
    // 两头都要看得见。
    const std::size_t keep = max_chars - 1;
    const std::size_t head = keep - keep / 2;  // 头多分一个
    const std::size_t tail = keep / 2;
    const std::size_t head_end = starts[head];
    const std::size_t tail_begin = tail == 0 ? text.size() : starts[starts.size() - tail];
    return text.substr(0, head_end) + "…" + text.substr(tail_begin);
}

std::string NormalizePathForCompare(const std::string& utf8_path) {
    if (utf8_path.empty()) {
        return std::string();
    }
    std::error_code ec;
    std::filesystem::path p = std::filesystem::weakly_canonical(Utf8Path(utf8_path), ec);
    if (ec || p.empty()) {
        p = Utf8Path(utf8_path).lexically_normal();
    }
    std::string s = PathToUtf8(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    // 大小写按 Windows 习惯不敏感(盘符、目录名都是 ASCII 场景居多;多字节
    // 字符的字节不落在 A-Z 区间,这个循环碰不着它们)。
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
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
// trace-aware 成对修补(逐枚追踪单)
// ---------------------------------------------------------------------------

TraceRepairReport RepairToolPairsWithTrace(std::vector<api::Message>& history,
                                            const agent::ToolExecutionLedger& ledger) {
    TraceRepairReport report;
    // 先按 trace 账修:扫全部 tool_use,查 ledger 里这枚 tool_use_id 的账。
    // Provider 给重复/空 tool_use id 时按 execution 序全列,不串账(单子:
    // 宿主不让两枚结果串账)。
    std::set<std::string> answered_by_trace;
    for (auto& message : history) {
        if (message.role != api::Role::Assistant) {
            continue;
        }
        std::vector<std::pair<std::string, api::ToolResultBlock>> patches;
        for (auto& block : message.content) {
            const auto* use = std::get_if<api::ToolUseBlock>(&block);
            if (use == nullptr || use->id.empty()) {
                continue;
            }
            const auto records = ledger.FindByToolUse(use->id);
            if (records.empty()) {
                continue;  // 没挂 trace 的(老档/内层),留给 legacy 补洞
            }
            answered_by_trace.insert(use->id);
            const agent::ToolExecutionRecord& record = *records.front();
            api::ToolResultBlock patch;
            patch.tool_use_id = use->id;
            patch.content = agent::BuildRecoveredResultText(record);
            patch.is_error = record.Classify() != agent::RecoveryClass::Finished ||
                             record.outcome != agent::ToolOutcome::Succeeded;
            switch (record.Classify()) {
                case agent::RecoveryClass::Finished:
                    if (record.outcome == agent::ToolOutcome::Succeeded &&
                        record.result_ref.kind == agent::ToolResultRef::Kind::Inline) {
                        ++report.result_recovered;
                    }
                    break;
                case agent::RecoveryClass::ResultRecoverable:
                    ++report.result_recovered;
                    break;
                case agent::RecoveryClass::UnknownAfterStart:
                    ++report.unknown_after_start;
                    break;
                case agent::RecoveryClass::NotStarted:
                    ++report.not_started;
                    break;
            }
            patches.emplace_back(use->id, std::move(patch));
        }
        if (patches.empty()) {
            continue;
        }
        // 补进紧随的 user 消息(tool_result 块排前头,anthropic 要求紧跟
        // tool_use);没有就插一条新 user 消息——与 legacy 同款形状。
        const bool next_is_user =
            (&message != &history.back()) && (&message + 1)->role == api::Role::User;
        if (next_is_user) {
            auto& target = (&message + 1)->content;
            // 已有同 id 的 result(没缺)不重复补。
            for (const auto& [id, patch] : patches) {
                const bool already = std::any_of(target.begin(), target.end(), [&id](const api::ContentBlock& b) {
                    const auto* result = std::get_if<api::ToolResultBlock>(&b);
                    return result != nullptr && result->tool_use_id == id;
                });
                if (already) {
                    continue;
                }
                target.insert(target.begin(), patch);
                ++report.repaired;
                ++report.trace_matched;
            }
        } else {
            api::Message filler;
            filler.role = api::Role::User;
            for (const auto& [id, patch] : patches) {
                filler.content.push_back(patch);
                ++report.repaired;
                ++report.trace_matched;
            }
            history.insert(std::next(history.begin(), std::distance(history.data(), &message) +
                                                        static_cast<std::ptrdiff_t>(1)),
                           std::move(filler));
        }
    }
    // legacy 兜底:其余孤儿(老档、trace 没挂上的)走老逻辑补洞。
    const int legacy = RepairToolPairs(history);
    report.repaired += legacy;
    return report;
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
        const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (!j.is_object()) {
            session.skipped_lines += 1;
            continue;
        }
        // 事件行顶层带 "type" 字段,消息行没有(消息行的 type 都在 content
        // 块里,一层之隔),靠这个分流。
        if (j.contains("type") && j["type"].is_string()) {
            const std::string type = j["type"].get<std::string>();
            if (type == "compact") {
                auto event = ParseCompactEvent(line);
                if (!event.has_value()) {
                    session.skipped_lines += 1;  // 坏事件行跳过,不废整场
                    continue;
                }
                session.compact_positions.push_back(session.all_messages.size());
                session.compact_count += 1;
                session.compact_epoch = session.compact_count;
                session.last_compact_manifest = nlohmann::json();
                session.messages = ApplyCompactEvent(std::move(session.messages), *event);
                continue;
            }
            if (type == "compact_v2") {
                // v2:回放与 v1 同型(archive + kept_from),manifest/epoch 记
                // 进 LoadedSession 供审计与 rebase。
                auto event = ParseCompactV2Event(line);
                if (!event.has_value()) {
                    session.skipped_lines += 1;
                    continue;
                }
                session.compact_positions.push_back(session.all_messages.size());
                session.compact_count += 1;
                session.compact_epoch = event->epoch > 0 ? event->epoch : session.compact_count;
                session.last_compact_manifest = event->manifest;
                session.messages = ApplyCompactEvent(std::move(session.messages), AsCompactEvent(*event));
                continue;
            }
            if (type == "title") {
                auto title = ParseTitleEvent(line);
                if (title.has_value()) {
                    session.title = std::move(*title);  // append-only,最后一条胜
                } else {
                    session.skipped_lines += 1;
                }
                continue;
            }
            if (type == "cwd") {
                auto cwd = ParseCwdEvent(line);
                if (cwd.has_value()) {
                    session.meta.cwd = std::move(*cwd);  // append-only,最后一条胜
                } else {
                    session.skipped_lines += 1;
                }
                continue;
            }
            if (type == "queue") {
                auto queued = ParseQueueEvent(line);
                if (queued.has_value()) {
                    session.queued_messages = std::move(*queued);  // 快照式,最后一条胜
                } else {
                    session.skipped_lines += 1;
                }
                continue;
            }
            if (type == "tool_trace_v1") {
                // 逐枚追踪:栅栏事件按文件序整收(账本折叠在 /resume 侧做,
                // 这里只管收行);坏行跳过不废整场(事件行通用约定)。
                auto traced = agent::ParseToolTraceEvent(line);
                if (traced.has_value()) {
                    session.tool_trace_events.push_back(std::move(*traced));
                } else {
                    session.skipped_lines += 1;
                }
                continue;
            }
            if (type.rfind("goal_", 0) == 0) {
                // 持久目标:goal_v1 族事件按文件序整收(状态机重建在
                // /resume 侧 GoalCoordinator::ReplayEvent);坏行跳过,老档
                // 空 = 没有 goal,消息账无损。
                auto goal_event = ParseGoalEvent(line);
                if (goal_event.has_value()) {
                    session.goal_events.push_back(std::move(*goal_event));
                } else {
                    session.skipped_lines += 1;
                }
                continue;
            }
            if (type == "mode_v1") {
                // Plan 模式单:最后一条胜,决定 resume 档位。坏行跳过——mode
                // 已写 Plan、这行坏了,按上一条有效 mode 恢复,不废整场。
                auto mode = ParseModeEvent(line);
                if (mode.has_value()) {
                    session.last_mode_event = std::move(*mode);
                } else {
                    session.skipped_lines += 1;
                }
                continue;
            }
            if (type == "plan_v1") {
                // 计划成品逐稿留账(全部收,/resume 侧按 plan_id 取最高
                // revision 并对审批做 stale 判定);坏行跳过不废整场。
                auto plan = ParsePlanEvent(line);
                if (plan.has_value()) {
                    session.plan_events.push_back(std::move(*plan));
                } else {
                    session.skipped_lines += 1;
                }
                continue;
            }
            if (type == "plan_review_v1") {
                auto review = ParsePlanReviewEvent(line);
                if (review.has_value()) {
                    session.last_plan_review = std::move(*review);
                } else {
                    session.skipped_lines += 1;
                }
                continue;
            }
            session.skipped_lines += 1;  // 认不得的事件类型,跳过
            continue;
        }
        auto message = MessageFromJson(j);
        if (message.has_value()) {
            session.all_messages.push_back(*message);
            session.messages.push_back(std::move(*message));
        } else {
            session.skipped_lines += 1;
        }
    }
    // 逐枚追踪单:有 trace 账的档先按四档结论修(能恢复原始结果的恢复,
    // unknown 标 unknown,未执行标未执行),legacy 补洞兜底;老档没 trace
    // 行,走的还是纯老逻辑,一个不坏。
    if (!session.tool_trace_events.empty()) {
        agent::ToolExecutionLedger ledger;
        for (const auto& event : session.tool_trace_events) {
            ledger.Fold(event);
        }
        const TraceRepairReport report = RepairToolPairsWithTrace(session.messages, ledger);
        session.repaired = report.repaired;
        session.trace_repair = report;
    } else {
        session.repaired = RepairToolPairs(session.messages);
    }
    return session;
}

std::string ExportSessionMarkdown(const SessionMeta& meta, const std::vector<api::Message>& messages,
                                   const std::string& session_id, int max_result_lines,
                                   const std::string& title,
                                   const std::vector<std::size_t>& compact_positions) {
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
    if (!title.empty()) {
        // /title 设过标题:标题当大标题,会话 id 降成一行元信息。
        out += "# " + title + "\n\n";
        out += "- 会话: " + session_id + "\n";
    } else {
        out += "# 会话 " + session_id + "\n\n";
    }
    out += "- 开始时间: " + (meta.started_at.empty() ? std::string("(未知)") : meta.started_at) + "\n";
    out += "- wire: " + meta.wire + "\n";
    out += "- model: " + meta.model + "\n";
    out += "- cwd: " + meta.cwd + "\n";

    // 压缩标注:compact_positions 升序,next_compact 指着下一个还没插的。
    std::size_t next_compact = 0;
    const auto emit_compact_notes = [&](std::size_t message_index) {
        while (next_compact < compact_positions.size() && compact_positions[next_compact] <= message_index) {
            out += "\n> ⚡ 此处发生过一次上下文压缩\n";
            ++next_compact;
        }
    };

    for (std::size_t mi = 0; mi < messages.size(); ++mi) {
        const auto& message = messages[mi];
        emit_compact_notes(mi);
        // 这条消息里有没有值得单开一节的正文(文本块)?只装着 tool_result
        // 的 user 消息不开"用户"节——结果已折进对应 tool_use 的 details。
        std::string text;
        for (const auto& block : message.content) {
            if (const auto* tb = std::get_if<api::TextBlock>(&block)) {
                if (!text.empty()) {
                    text += "\n";
                }
                text += tb->text;
            } else if (const auto* image = std::get_if<api::ImageBlock>(&block)) {
                if (!text.empty()) {
                    text += "\n";
                }
                text += "[图片] " + image->filename + " (" + std::to_string(image->width) + "x" +
                        std::to_string(image->height) + ")";
            } else if (const auto* generated = std::get_if<api::ModelImageBlock>(&block)) {
                // 模型输出图片:导出里给引用(文件在会话 images/ 目录),
                // 不嵌 base64。
                if (!text.empty()) {
                    text += "\n";
                }
                text += "[模型图片] " + generated->path + " (" + std::to_string(generated->width) + "x" +
                        std::to_string(generated->height) + ", " + std::to_string(generated->bytes) + " 字节)";
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
    // 压缩发生在最后一条消息之后(极少见,但位置合法):标注补在末尾。
    emit_compact_notes(messages.size());
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

bool SessionStore::AppendCompactEvent(const CompactEvent& event) {
    if (!out_.is_open()) {
        return false;
    }
    out_ << SerializeCompactEvent(event, NowTimestamp()) << "\n";
    out_.flush();
    return out_.good();
}

bool SessionStore::AppendCompactV2Event(const CompactV2Event& event) {
    if (!out_.is_open()) {
        return false;
    }
    out_ << SerializeCompactV2Event(event, NowTimestamp()) << "\n";
    out_.flush();
    return out_.good();
}

bool SessionStore::AppendTitleEvent(const std::string& title) {
    if (!out_.is_open()) {
        return false;
    }
    out_ << SerializeTitleEvent(title, NowTimestamp()) << "\n";
    out_.flush();
    return out_.good();
}

bool SessionStore::AppendCwdEvent(const std::string& cwd) {
    if (!out_.is_open()) {
        return false;
    }
    out_ << SerializeCwdEvent(cwd, NowTimestamp()) << "\n";
    out_.flush();
    return out_.good();
}

bool SessionStore::AppendQueueEvent(const std::vector<ArchivedQueueItem>& items) {
    if (!out_.is_open()) {
        return false;
    }
    out_ << SerializeQueueEvent(items, NowTimestamp()) << "\n";
    out_.flush();
    return out_.good();
}

bool SessionStore::AppendToolTraceEvent(const agent::ToolTraceEvent& event) {
    if (!out_.is_open()) {
        return false;
    }
    out_ << agent::SerializeToolTraceEvent(event, NowTimestamp()) << "\n";
    out_.flush();
    return out_.good();
}

bool SessionStore::AppendRawLine(const std::string& json_line) {
    if (!out_.is_open()) {
        return false;
    }
    out_ << json_line << "\n";
    out_.flush();
    return out_.good();
}

bool SessionStore::AppendGoalEvent(const GoalSessionEvent& event) {
    if (!out_.is_open()) {
        return false;
    }
    out_ << SerializeGoalEvent(event, NowTimestamp()) << "\n";
    out_.flush();
    return out_.good();
}

bool SessionStore::AppendModeEvent(const ModeEvent& event) {
    if (!out_.is_open()) {
        return false;
    }
    out_ << SerializeModeEvent(event, NowTimestamp()) << "\n";
    out_.flush();
    return out_.good();
}

bool SessionStore::AppendGoalEvidence(const GoalEvidenceRecord& evidence) {
    if (!out_.is_open()) {
        return false;
    }
    out_ << SerializeGoalEvidence(evidence, NowTimestamp()) << "\n";
    out_.flush();
    return out_.good();
}

bool SessionStore::AppendPlanEvent(const PlanEvent& event) {
    if (!out_.is_open()) {
        return false;
    }
    out_ << SerializePlanEvent(event, NowTimestamp()) << "\n";
    out_.flush();
    return out_.good();
}

bool SessionStore::AppendPlanReviewEvent(const PlanReviewEvent& event) {
    if (!out_.is_open()) {
        return false;
    }
    out_ << SerializePlanReviewEvent(event, NowTimestamp()) << "\n";
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

namespace {

// 只读文件第一行(meta 行),cwd 过滤用——不匹配的场子不必整个读进来。
std::optional<std::string> ReadFirstLine(const std::string& file_path) {
    std::ifstream file(Utf8Path(file_path), std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::string line;
    if (!std::getline(file, line)) {
        return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

}  // namespace

std::vector<SessionListEntry> ListSessions(const std::string& sessions_dir, std::size_t limit,
                                            const std::string& cwd_filter) {
    namespace fs = std::filesystem;
    std::vector<SessionListEntry> candidates;
    std::error_code ec;
    fs::directory_iterator it(Utf8Path(sessions_dir), ec);
    if (ec) {
        return candidates;  // 目录不存在:还没存过任何会话
    }
    for (const auto& dir_entry : it) {
        if (!dir_entry.is_regular_file(ec) || dir_entry.path().extension() != ".jsonl") {
            continue;
        }
        SessionListEntry entry;
        entry.id = PathToUtf8(dir_entry.path().stem());
        entry.file_path = PathToUtf8(dir_entry.path());
        candidates.push_back(std::move(entry));
    }
    // id 以 yyyymmdd-HHMMSS 起头,字典倒序即时间倒序。
    std::sort(candidates.begin(), candidates.end(),
              [](const SessionListEntry& a, const SessionListEntry& b) { return a.id > b.id; });

    // cwd 过滤:只读每个候选的 meta 行来比对,凑够 limit 场就收手。
    const std::string want = cwd_filter.empty() ? std::string() : NormalizePathForCompare(cwd_filter);
    std::vector<SessionListEntry> entries;
    for (auto& candidate : candidates) {
        if (entries.size() >= limit) {
            break;
        }
        if (!want.empty()) {
            const auto first_line = ReadFirstLine(candidate.file_path);
            if (!first_line.has_value()) {
                continue;
            }
            const auto meta = ParseSessionMeta(*first_line);
            if (!meta.has_value() || NormalizePathForCompare(meta->cwd) != want) {
                continue;  // 读不出 meta 或不是本目录的场子,过滤模式下不列
            }
        }
        entries.push_back(std::move(candidate));
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
                    entry.cwd = meta->cwd;
                    continue;  // meta 行不算消息
                }
                // 首行不是 meta:旧格式/坏文件,照样往下数
            }
            // 事件行(顶层 type 字段)不算消息;title 事件取最后一条。
            // 先用子串粗筛(省得每行都过一遍 JSON 解析),撞上了再真解析
            // 验顶层 type——消息行万一在嵌套的 tool_use input 里带同样字样,
            // 解析这关过不了,照旧按消息数。
            if (line.find("\"type\":\"title\"") != std::string::npos) {
                if (auto title = ParseTitleEvent(line); title.has_value()) {
                    entry.title = std::move(*title);
                    continue;
                }
            }
            if (line.find("\"type\":\"compact\"") != std::string::npos) {
                if (ParseCompactEvent(line).has_value()) {
                    continue;
                }
            }
            if (line.find("\"type\":\"cwd\"") != std::string::npos) {
                if (ParseCwdEvent(line).has_value()) {
                    continue;  // 事件行不算消息
                }
            }
            if (line.find("\"type\":\"queue\"") != std::string::npos) {
                if (ParseQueueEvent(line).has_value()) {
                    continue;  // 排队快照事件行,同样不算消息
                }
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
                        if (const auto* image = std::get_if<api::ImageBlock>(&block); image != nullptr) {
                            entry.first_user_text = "[图片] " + image->filename;
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

std::vector<PromptHistoryRecord> ExtractPromptHistory(const std::string& jsonl_content) {
    std::vector<PromptHistoryRecord> out;
    std::size_t line_start = 0;
    while (line_start <= jsonl_content.size()) {
        const std::size_t line_end = jsonl_content.find('\n', line_start);
        const std::string line =
            jsonl_content.substr(line_start,
                                 line_end == std::string::npos ? std::string::npos : line_end - line_start);
        if (!line.empty()) {
            try {
                const nlohmann::json parsed = nlohmann::json::parse(line);
                // 消息行没有顶层 type;事件行(compact/title/cwd)有,跳过。
                const bool is_message = parsed.is_object() && !parsed.contains("type") &&
                                        parsed.contains("role") && parsed.contains("content");
                if (is_message && parsed["role"] == "user" && parsed["content"].is_array() &&
                    !parsed["content"].empty()) {
                    const auto& first = parsed["content"][0];
                    bool has_text_only = first.is_object() && first.value("type", "") == "text" &&
                                         first.contains("text") && first["text"].is_string();
                    if (has_text_only) {
                        // tool_result 装在 user 消息里(工具回执),不是提问。
                        for (const auto& block : parsed["content"]) {
                            if (block.is_object() && block.value("type", "") == "tool_result") {
                                has_text_only = false;
                                break;
                            }
                        }
                    }
                    if (has_text_only) {
                        std::string text = first["text"].get<std::string>();
                        // 首尾空白剥掉;空白串、slash 命令不是提问。
                        while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                                                 text.front() == '\n' || text.front() == '\r')) {
                            text.erase(text.begin());
                        }
                        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                                                 text.back() == '\n' || text.back() == '\r')) {
                            text.pop_back();
                        }
                        // 定时循环 tick 的 scheduled message(loop 单):定时
                        // 触发的重复句不算提问,不进 Ctrl+R
                        // 历史搜索(免得满屏重复)。
                        if (!text.empty() && text.front() != '/' &&
                            !text.starts_with("[定时循环 tick]") &&
                            !text.starts_with("[goal ")) {
                            out.push_back(PromptHistoryRecord{std::move(text),
                                                              parsed.value("ts", std::string())});
                        }
                    }
                }
            } catch (const std::exception&) {
                // 坏行跳过,不废整份(与 ParseSessionFile 同一取舍)。
            }
        }
        if (line_end == std::string::npos) {
            break;
        }
        line_start = line_end + 1;
    }
    return out;
}

}  // namespace lubancode::sessions
