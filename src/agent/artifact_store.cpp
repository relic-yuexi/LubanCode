#include "agent/artifact_store.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

#include "agent/context_events.hpp"  // Fingerprint64:块局部指纹
#include "agent/session_store.hpp"   // NowTimestamp
#include "hooks/hash.hpp"            // Sha256Hex:真本的内容寻址
#include "platform/text_encoding.hpp"

namespace lubancode::agent {

namespace {

// UTF-8:这个字节位置是不是一个码点的起点(续字节 10xxxxxx 不是)。
bool IsCodePointStart(const std::string& text, std::size_t index) {
    if (index >= text.size()) {
        return true;
    }
    return (static_cast<unsigned char>(text[index]) & 0xC0) != 0x80;
}

// 行表:每行的 [byte_start, byte_end)(不含换行符),行本身完整不劈码点。
struct LineTable {
    struct Line {
        std::size_t start;
        std::size_t end;      // 不含行尾换行
        std::size_t end_with_newline;
    };
    std::vector<Line> lines;
};
LineTable BuildLineTable(const std::string& content) {
    LineTable table;
    std::size_t start = 0;
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n') {
            table.lines.push_back({start, i, i + 1});
            start = i + 1;
        }
    }
    if (start < content.size()) {
        table.lines.push_back({start, content.size(), content.size()});
    }
    return table;
}

std::string_view LineText(const std::string& content, const LineTable::Line& line) {
    return std::string_view(content).substr(line.start, line.end - line.start);
}

// ASCII 小写化(中文等多字节原样;大小写折叠只对 ASCII,检索口径够用)。
char LowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string LowerAsciiCopy(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out += LowerAscii(c);
    }
    return out;
}

// 大括号深度(源码/格式化 JSON 的分块边界参考)。字符串/字符字面量与
// 注释里的括号不精确排除——分块只是检索粒度,不是语法分析,粗够了。
int BraceDelta(std::string_view line) {
    int delta = 0;
    for (const char c : line) {
        if (c == '{') {
            ++delta;
        } else if (c == '}') {
            --delta;
        }
    }
    return delta;
}

bool LooksLikeMarkdownHeading(std::string_view line) {
    return line.size() > 1 && line[0] == '#' &&
           (line[1] == ' ' || line[1] == '#' || line[1] == '\t');
}

}  // namespace

ArtifactContentKind DetectArtifactKind(const std::string& tool_name, const std::string& content) {
    if (tool_name == "run_command" || tool_name == "run_in_background") {
        return ArtifactContentKind::CommandLog;
    }
    // JSON:首个非空白字符是 { 或 [,且整体解析得动。
    std::size_t first = content.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && (content[first] == '{' || content[first] == '[')) {
        try {
            (void)nlohmann::json::parse(content);
            return ArtifactContentKind::Json;
        } catch (...) {
            // 解析不动:当普通文本。
        }
    }
    // Markdown:前 50 行里有标题行。
    const LineTable table = BuildLineTable(content);
    const std::size_t scan = std::min<std::size_t>(table.lines.size(), 50);
    for (std::size_t i = 0; i < scan; ++i) {
        if (LooksLikeMarkdownHeading(LineText(content, table.lines[i]))) {
            return ArtifactContentKind::Markdown;
        }
    }
    return ArtifactContentKind::Text;
}

nlohmann::json ArtifactRef::ToJson() const {
    nlohmann::json json;
    json["artifact_id"] = artifact_id;
    json["session_id"] = session_id;
    json["tool_use_id"] = tool_use_id;
    json["tool_name"] = tool_name;
    json["mime"] = mime;
    json["encoding"] = encoding;
    json["bytes"] = bytes;
    json["lines"] = lines;
    json["sha256"] = sha256;
    json["created_at"] = created_at;
    json["source_message_index"] = source_message_index;
    json["preview"] = preview;
    json["blob_path"] = blob_path;
    json["chunk_index_path"] = chunk_index_path;
    return json;
}

std::optional<ArtifactRef> ArtifactRef::FromJson(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("artifact_id") || !json["artifact_id"].is_string() ||
        !json.contains("tool_use_id") || !json["tool_use_id"].is_string() || !json.contains("sha256") ||
        !json["sha256"].is_string()) {
        return std::nullopt;
    }
    ArtifactRef ref;
    ref.artifact_id = json["artifact_id"].get<std::string>();
    ref.tool_use_id = json["tool_use_id"].get<std::string>();
    ref.sha256 = json["sha256"].get<std::string>();
    auto str = [&json](const char* key) { return json.value(key, std::string()); };
    ref.session_id = str("session_id");
    ref.tool_name = str("tool_name");
    ref.mime = str("mime");
    ref.encoding = str("encoding");
    ref.created_at = str("created_at");
    ref.preview = str("preview");
    ref.blob_path = str("blob_path");
    ref.chunk_index_path = str("chunk_index_path");
    if (json.contains("bytes") && json["bytes"].is_number_unsigned()) {
        ref.bytes = json["bytes"].get<std::size_t>();
    }
    if (json.contains("lines") && json["lines"].is_number_unsigned()) {
        ref.lines = json["lines"].get<std::size_t>();
    }
    if (json.contains("source_message_index") && json["source_message_index"].is_number_unsigned()) {
        ref.source_message_index = json["source_message_index"].get<std::size_t>();
    }
    return ref;
}

nlohmann::json ArtifactChunk::ToJson() const {
    nlohmann::json json;
    json["chunk_id"] = chunk_id;
    json["line_start"] = line_start;
    json["line_count"] = line_count;
    json["byte_start"] = byte_start;
    json["byte_end"] = byte_end;
    json["hash"] = hash;
    json["heading"] = heading;
    return json;
}

std::optional<ArtifactChunk> ArtifactChunk::FromJson(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("chunk_id") || !json["chunk_id"].is_string()) {
        return std::nullopt;
    }
    ArtifactChunk chunk;
    chunk.chunk_id = json["chunk_id"].get<std::string>();
    auto num = [&json](const char* key) -> std::size_t {
        return json.contains(key) && json[key].is_number_unsigned() ? json[key].get<std::size_t>() : 0;
    };
    chunk.line_start = num("line_start");
    chunk.line_count = num("line_count");
    chunk.byte_start = num("byte_start");
    chunk.byte_end = num("byte_end");
    chunk.hash = json.value("hash", std::string());
    chunk.heading = json.value("heading", std::string());
    return chunk;
}

std::vector<ArtifactChunk> ChunkArtifact(const std::string& content, ArtifactContentKind kind,
                                         std::size_t target_bytes) {
    std::vector<ArtifactChunk> chunks;
    if (content.empty()) {
        return chunks;
    }
    const LineTable table = BuildLineTable(content);
    if (table.lines.empty()) {
        return chunks;
    }

    // 候选边界集合(行下标,0 起):分块只落在这些行首,行内绝不切。
    std::vector<std::size_t> boundaries;
    const auto add_boundary = [&boundaries](std::size_t line_index) {
        if (std::find(boundaries.begin(), boundaries.end(), line_index) == boundaries.end()) {
            boundaries.push_back(line_index);
        }
    };
    add_boundary(0);
    add_boundary(table.lines.size());

    int depth = 0;
    for (std::size_t i = 0; i < table.lines.size(); ++i) {
        const std::string_view line = LineText(content, table.lines[i]);
        const std::size_t trimmed = line.find_first_not_of(" \t");
        const std::string_view stripped = trimmed == std::string_view::npos
                                              ? std::string_view{}
                                              : line.substr(trimmed);
        switch (kind) {
            case ArtifactContentKind::Markdown:
                if (LooksLikeMarkdownHeading(stripped)) {
                    add_boundary(i);
                }
                break;
            case ArtifactContentKind::SourceCode:
                depth += BraceDelta(line);
                if (depth <= 0) {
                    depth = 0;
                    add_boundary(i + 1);  // 顶层结构收口处(函数/类结束)开新块
                }
                break;
            case ArtifactContentKind::Json: {
                const int delta = BraceDelta(line);
                if (depth + delta <= 1 && (depth > 0 || delta > 0)) {
                    // 顶层元素结束/开始的行当边界(格式化 JSON 每个顶层元素
                    // 一段;单行 JSON 落不进任何边界,自成一块)。
                }
                depth += delta;
                if (depth <= 0) {
                    depth = 0;
                    add_boundary(i + 1);
                }
                break;
            }
            case ArtifactContentKind::Text:
            case ArtifactContentKind::CommandLog:
                break;
        }
    }
    std::sort(boundaries.begin(), boundaries.end());

    // 按边界切,相邻段合并到 target_bytes 为止;超长段硬按行窗再切。
    const auto push_chunk = [&content, &table, &chunks, kind](std::size_t from_line, std::size_t to_line) {
        if (from_line >= to_line) {
            return;
        }
        ArtifactChunk chunk;
        chunk.chunk_id = "c" + std::to_string(chunks.size());
        chunk.line_start = from_line + 1;  // 1 起
        chunk.line_count = to_line - from_line;
        chunk.byte_start = table.lines[from_line].start;
        chunk.byte_end = table.lines[to_line - 1].end_with_newline;
        chunk.hash = Fingerprint64(content.substr(chunk.byte_start, chunk.byte_end - chunk.byte_start));
        // 语义标题:Markdown 取首个标题行;源码取首个非空行;其余留空。
        for (std::size_t i = from_line; i < to_line; ++i) {
            const std::string_view line = LineText(content, table.lines[i]);
            if (line.empty()) {
                continue;
            }
            const std::size_t trimmed = line.find_first_not_of(" \t");
            const std::string_view stripped = trimmed == std::string_view::npos
                                                  ? std::string_view{}
                                                  : line.substr(trimmed);
            if (kind == ArtifactContentKind::Markdown && !chunk.heading.empty()) {
                break;  // 只认块内第一个标题
            }
            if (kind == ArtifactContentKind::Markdown && !LooksLikeMarkdownHeading(stripped)) {
                continue;
            }
            chunk.heading = std::string(stripped.substr(0, std::min<std::size_t>(stripped.size(), 60)));
            break;
        }
        chunks.push_back(std::move(chunk));
    };

    for (std::size_t b = 0; b + 1 < boundaries.size(); ++b) {
        const std::size_t from = boundaries[b];
        const std::size_t to = boundaries[b + 1];
        const std::size_t span_bytes =
            table.lines[to - 1].end_with_newline - table.lines[from].start;
        if (span_bytes <= target_bytes) {
            push_chunk(from, to);
            continue;
        }
        // 超长段:按行窗硬切(仍在行首,UTF-8 天然安全)。
        std::size_t window_start = from;
        std::size_t used = 0;
        for (std::size_t i = from; i < to; ++i) {
            const std::size_t line_bytes = table.lines[i].end_with_newline - table.lines[i].start;
            if (used + line_bytes > target_bytes && i > window_start) {
                push_chunk(window_start, i);
                window_start = i;
                used = 0;
            }
            used += line_bytes;
        }
        push_chunk(window_start, to);
    }
    return chunks;
}

std::vector<ArtifactSearchHit> SearchArtifactContent(const std::string& content,
                                                     const std::vector<ArtifactChunk>& chunks,
                                                     const std::string& query, int max_results) {
    std::vector<ArtifactSearchHit> hits;
    if (query.empty() || content.empty() || max_results <= 0) {
        return hits;
    }
    const std::string needle = LowerAsciiCopy(query);
    const LineTable table = BuildLineTable(content);
    // 行号 -> chunk_id(块表给过才填)。
    std::map<std::size_t, std::string> chunk_of_line;
    for (const auto& chunk : chunks) {
        for (std::size_t line = chunk.line_start; line < chunk.line_start + chunk.line_count; ++line) {
            chunk_of_line[line] = chunk.chunk_id;
        }
    }
    for (std::size_t i = 0; i < table.lines.size(); ++i) {
        const std::string_view line = LineText(content, table.lines[i]);
        if (line.empty() || line.size() < needle.size()) {
            continue;
        }
        const std::string lowered = LowerAsciiCopy(std::string(line));
        int count = 0;
        for (std::size_t pos = lowered.find(needle); pos != std::string::npos;
             pos = lowered.find(needle, pos + needle.size())) {
            ++count;
        }
        if (count == 0) {
            continue;
        }
        ArtifactSearchHit hit;
        hit.line = i + 1;
        hit.score = count;
        if (auto it = chunk_of_line.find(hit.line); it != chunk_of_line.end()) {
            hit.chunk_id = it->second;
        }
        // 预览:命中处前后各扩 ~40 字节,退到码点边界。
        const std::size_t at = lowered.find(needle);
        std::size_t window_begin = at > 40 ? at - 40 : 0;
        std::size_t window_end = at + needle.size() + 40;
        if (window_end > line.size()) {
            window_end = line.size();
        }
        while (window_begin < window_end && !IsCodePointStart(content, table.lines[i].start + window_begin)) {
            ++window_begin;  // 起点推到码点边界
        }
        while (window_end > window_begin && window_end < line.size() &&
               !IsCodePointStart(content, table.lines[i].start + window_end)) {
            --window_end;  // 终点退到码点边界
        }
        hit.snippet = std::string(line.substr(window_begin, window_end - window_begin));
        hits.push_back(std::move(hit));
    }
    std::stable_sort(hits.begin(), hits.end(), [](const ArtifactSearchHit& a, const ArtifactSearchHit& b) {
        return a.score > b.score;
    });
    if (hits.size() > static_cast<std::size_t>(max_results)) {
        hits.resize(static_cast<std::size_t>(max_results));
    }
    return hits;
}

// ---------------------------------------------------------------------------
// 磁盘薄壳
// ---------------------------------------------------------------------------

namespace {

std::string MimeFor(ArtifactContentKind kind) {
    switch (kind) {
        case ArtifactContentKind::Markdown:
            return "text/markdown; charset=utf-8";
        case ArtifactContentKind::Json:
            return "application/json; charset=utf-8";
        default:
            return "text/plain; charset=utf-8";
    }
}

// 原子写:同目录 tmp 文件写完 flush/close 再 rename。任一步失败 false。
bool AtomicWriteFile(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return false;
        }
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.flush();
        if (!file.good()) {
            file.close();
            std::remove(tmp.c_str());
            return false;
        }
    }
    // Windows 的 rename 不覆盖已存在目标:先删再改名(同目录,原子性够)。
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool AppendLineFlushed(const std::string& path, const std::string& line) {
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file.is_open()) {
        return false;
    }
    file << line << "\n";
    file.flush();
    return file.good();
}

std::optional<std::string> ReadFileBytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

}  // namespace

bool ContextArtifactStore::Open(std::string root_dir, std::string session_id) {
    active_ = false;
    refs_.clear();
    next_seq_ = 1;
    std::error_code ec;
    const std::filesystem::path root(root_dir);
    std::filesystem::create_directories(root / "blobs", ec);
    if (ec) {
        return false;
    }
    std::filesystem::create_directories(root / "chunks", ec);
    if (ec) {
        return false;
    }
    // 读回已有索引(append-only,编号接着走)。
    const std::string index_path = (root / "index.jsonl").string();
    if (const auto bytes = ReadFileBytes(index_path); bytes.has_value()) {
        std::istringstream stream(*bytes);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty()) {
                continue;
            }
            try {
                const nlohmann::json parsed = nlohmann::json::parse(line);
                if (auto ref = ArtifactRef::FromJson(parsed); ref.has_value()) {
                    const unsigned long long seq = std::strtoull(ref->artifact_id.c_str() + 1, nullptr, 10);
                    if (seq >= next_seq_) {
                        next_seq_ = seq + 1;
                    }
                    refs_.push_back(std::move(*ref));
                }
            } catch (...) {
                // 坏行跳过:索引可从 blob/JSONL 重建,单行坏了不废仓。
            }
        }
    }
    root_ = std::move(root_dir);
    session_id_ = std::move(session_id);
    active_ = true;
    return true;
}

std::optional<ArtifactRef> ContextArtifactStore::Offload(const std::string& tool_use_id,
                                                         const std::string& tool_name,
                                                         const std::string& content,
                                                         std::size_t source_message_index) {
    if (!active_ || tool_use_id.empty() || content.empty()) {
        return std::nullopt;
    }
    // 幂等:同 tool_use_id 已卸过,还旧 ref(请求视图重放同一引用)。
    for (const auto& existing : refs_) {
        if (existing.tool_use_id == tool_use_id) {
            return existing;
        }
    }

    const std::string sha = lubancode::hooks::Sha256Hex(content);
    const ArtifactContentKind kind = DetectArtifactKind(tool_name, content);
    const LineTable table = BuildLineTable(content);

    ArtifactRef ref;
    char id_buffer[16];
    std::snprintf(id_buffer, sizeof(id_buffer), "a%04llu",
                  static_cast<unsigned long long>(next_seq_));
    ref.artifact_id = id_buffer;
    ref.session_id = session_id_;
    ref.tool_use_id = tool_use_id;
    ref.tool_name = tool_name;
    ref.mime = MimeFor(kind);
    ref.bytes = content.size();
    ref.lines = table.lines.size();
    ref.sha256 = sha;
    ref.created_at = NowTimestamp();
    ref.source_message_index = source_message_index;
    // 预览:首行(剥空白,截 80 字节,码点安全)。
    {
        std::string first_line = table.lines.empty() ? std::string() : std::string(LineText(content, table.lines[0]));
        while (!first_line.empty() && (first_line.back() == '\r' || first_line.back() == ' ')) {
            first_line.pop_back();
        }
        if (first_line.size() > 80) {
            first_line.resize(80);
            while (!first_line.empty() && (static_cast<unsigned char>(first_line.back()) & 0xC0) == 0x80) {
                first_line.pop_back();
            }
        }
        ref.preview = std::move(first_line);
    }
    ref.blob_path = "blobs/" + sha + ".txt";
    ref.chunk_index_path = "chunks/" + sha + ".json";

    const std::filesystem::path root(root_);
    // 次序钉死:blob -> chunks -> index。任一步失败给 nullopt,不追加索引,
    // 编号不推进(下一枚接着用同一个号——刚失败那枚没有留下登记行)。
    const std::string blob_path = (root / ref.blob_path).string();
    if (!std::filesystem::exists(blob_path)) {
        if (!AtomicWriteFile(blob_path, content)) {
            return std::nullopt;
        }
    }
    if (!std::filesystem::exists((root / ref.chunk_index_path).string())) {
        nlohmann::json chunk_index;
        chunk_index["sha256"] = sha;
        chunk_index["kind"] = [kind]() {
            switch (kind) {
                case ArtifactContentKind::Markdown:
                    return "markdown";
                case ArtifactContentKind::Json:
                    return "json";
                case ArtifactContentKind::SourceCode:
                    return "source";
                case ArtifactContentKind::CommandLog:
                    return "log";
                case ArtifactContentKind::Text:
                    return "text";
            }
            return "text";
        }();
        nlohmann::json chunk_array = nlohmann::json::array();
        for (const auto& chunk : ChunkArtifact(content, kind)) {
            chunk_array.push_back(chunk.ToJson());
        }
        chunk_index["chunks"] = std::move(chunk_array);
        if (!AtomicWriteFile((root / ref.chunk_index_path).string(), chunk_index.dump())) {
            return std::nullopt;
        }
    }
    if (!AppendLineFlushed((root / "index.jsonl").string(), ref.ToJson().dump())) {
        return std::nullopt;  // blob 在了但没登记:不可追回,调用方保留内存全文
    }
    refs_.push_back(ref);
    ++next_seq_;
    return ref;
}

const ArtifactRef* ContextArtifactStore::Find(const std::string& artifact_id) const {
    for (const auto& ref : refs_) {
        if (ref.artifact_id == artifact_id) {
            return &ref;
        }
    }
    return nullptr;
}

std::optional<std::string> ContextArtifactStore::ReadBlobVerified(const ArtifactRef& ref,
                                                                  std::string* error) const {
    const auto bytes = ReadFileBytes((std::filesystem::path(root_) / ref.blob_path).string());
    if (!bytes.has_value()) {
        if (error != nullptr) {
            *error = "artifact " + ref.artifact_id + " 的 blob 读不到: " + ref.blob_path;
        }
        return std::nullopt;
    }
    if (lubancode::hooks::Sha256Hex(*bytes) != ref.sha256) {
        if (error != nullptr) {
            *error = "artifact " + ref.artifact_id + " 的 hash 不合(文件被改/被截),已隔离,不供内容";
        }
        return std::nullopt;
    }
    return bytes;
}

std::vector<ArtifactChunk> ContextArtifactStore::ChunksFor(const ArtifactRef& ref) const {
    const auto bytes = ReadFileBytes((std::filesystem::path(root_) / ref.chunk_index_path).string());
    if (!bytes.has_value()) {
        return {};
    }
    try {
        const nlohmann::json parsed = nlohmann::json::parse(*bytes);
        std::vector<ArtifactChunk> chunks;
        if (parsed.contains("chunks") && parsed["chunks"].is_array()) {
            for (const auto& item : parsed["chunks"]) {
                if (auto chunk = ArtifactChunk::FromJson(item); chunk.has_value()) {
                    chunks.push_back(std::move(*chunk));
                }
            }
        }
        return chunks;
    } catch (...) {
        return {};
    }
}

ContextArtifactStore::ReadResult ContextArtifactStore::Read(const ArtifactRef& ref, const std::string& chunk_id,
                                                            std::size_t line_start, std::size_t line_count,
                                                            std::size_t max_bytes) const {
    ReadResult result;
    std::string error;
    const auto content = ReadBlobVerified(ref, &error);
    if (!content.has_value()) {
        result.error = error;
        return result;
    }
    const LineTable table = BuildLineTable(*content);
    if (table.lines.empty()) {
        result.error = "artifact 内容为空";
        return result;
    }
    const std::vector<ArtifactChunk> chunks = ChunksFor(ref);
    const std::string available = "共 " + std::to_string(table.lines.size()) + " 行;可用行 1-" +
                                  std::to_string(table.lines.size()) +
                                  (chunks.empty() ? std::string()
                                                  : ",块 c000-c" +
                                                        std::to_string(chunks.size() - 1));

    std::size_t from_line = 0;  // 0 起
    std::size_t to_line = table.lines.size();
    if (!chunk_id.empty()) {
        const ArtifactChunk* found = nullptr;
        for (const auto& chunk : chunks) {
            if (chunk.chunk_id == chunk_id) {
                found = &chunk;
                break;
            }
        }
        if (found == nullptr) {
            result.error = "没有块 " + chunk_id;
            result.available = available;
            return result;
        }
        from_line = found->line_start - 1;
        to_line = from_line + found->line_count;
        result.chunk_id = chunk_id;
    } else {
        if (line_start == 0 || line_start > table.lines.size()) {
            result.error = "line_start 越界(1 起)";
            result.available = available;
            return result;
        }
        from_line = line_start - 1;
        if (line_count == 0) {
            to_line = table.lines.size();
        } else {
            to_line = std::min(table.lines.size(), from_line + line_count);
        }
    }
    const std::size_t span_bytes = table.lines[to_line - 1].end_with_newline - table.lines[from_line].start;
    if (span_bytes > max_bytes) {
        // 超预算:拒绝并给可用范围,不悄悄截(规格"读太大便拒绝")。
        std::size_t fit = from_line;
        std::size_t used = 0;
        for (std::size_t i = from_line; i < to_line; ++i) {
            const std::size_t line_bytes = table.lines[i].end_with_newline - table.lines[i].start;
            if (used + line_bytes > max_bytes) {
                break;
            }
            used += line_bytes;
            fit = i + 1;
        }
        result.error = "请求范围 " + std::to_string(span_bytes) + " 字节,超出单次读取预算 " +
                       std::to_string(max_bytes) + " 字节";
        result.available = "这个范围最多一次读 " + std::to_string(fit - from_line) + " 行;" + available;
        return result;
    }
    result.ok = true;
    result.line_start = from_line + 1;
    result.line_count = to_line - from_line;
    result.text = content->substr(table.lines[from_line].start,
                                  table.lines[to_line - 1].end_with_newline - table.lines[from_line].start);
    return result;
}

std::optional<std::vector<ArtifactSearchHit>> ContextArtifactStore::Search(const ArtifactRef& ref,
                                                                           const std::string& query,
                                                                           int max_results,
                                                                           std::string* error) const {
    std::string read_error;
    const auto content = ReadBlobVerified(ref, &read_error);
    if (!content.has_value()) {
        if (error != nullptr) {
            *error = read_error;
        }
        return std::nullopt;
    }
    return SearchArtifactContent(*content, ChunksFor(ref), query, max_results);
}

ContextArtifactStore::Stats ContextArtifactStore::StatsOf() const {
    Stats stats;
    stats.artifacts = refs_.size();
    std::map<std::string, bool> seen_blobs;
    for (const auto& ref : refs_) {
        if (!seen_blobs[ref.blob_path]) {
            seen_blobs[ref.blob_path] = true;
            stats.total_bytes += ref.bytes;
        }
    }
    return stats;
}

}  // namespace lubancode::agent
