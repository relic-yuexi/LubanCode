// frontmatter.hpp 的实现。emit 侧自己定引号策略(空串、null/bool/数字
// 样子、引导指示符、": "、" #"、控制字节都加双引号,其余走 plain),parse
// 侧一律取 node 原文(Scalar()),两边合起来保证往返字节稳定。

#include "memory/frontmatter.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <yaml-cpp/yaml.h>

#include "platform/text_encoding.hpp"

namespace lubancode::memory::frontmatter {

namespace {

// 标题/摘要的单行化上限(与 project_memory 的主题上限同一量级)。
constexpr std::size_t kMaxTitleBytes = 200;

std::string TrimWhitespace(std::string value) {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t begin = 0;
    while (begin < value.size() && is_space(value[begin])) ++begin;
    std::size_t end = value.size();
    while (end > begin && is_space(value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

std::string OneLine(std::string value, std::size_t max_bytes) {
    for (char& c : value) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    value = TrimWhitespace(std::move(value));
    if (value.size() > max_bytes) {
        value.resize(lubancode::platform::Utf8PrefixBoundary(value, max_bytes));
        value += "...";
    }
    return value;
}

// ---- emit 侧的引号策略 ----

bool IsControlByte(char c) {
    const unsigned char byte = static_cast<unsigned char>(c);
    return byte < 0x20 || byte == 0x7F;
}

// 会按 null / 布尔解析的词(YAML 1.1 一族,yaml-cpp 读裸词认这些)。
bool LooksLikeSpecialWord(const std::string& s) {
    static constexpr const char* words[] = {
        "null", "Null", "NULL", "~", "true", "True", "TRUE", "false", "False", "FALSE",
        "yes",  "Yes",  "YES",  "no", "No",   "NO",   "on",    "On",    "ON",   "off",
        "Off",  "OFF",
    };
    for (const char* word : words) {
        if (s == word) return true;
    }
    return false;
}

// 十进制/十六进制/八进制整数与小数(含指数)。宁可多引不可漏引——引号只
// 影响长相,漏了会改语义。
bool LooksLikeNumber(const std::string& s) {
    std::size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    if (i >= s.size()) return false;
    if (s.size() - i > 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) return true;
    if (s.size() - i > 2 && s[i] == '0' && (s[i + 1] == 'o' || s[i + 1] == 'O')) return true;
    bool digits = false;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) != 0) {
        ++i;
        digits = true;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) != 0) {
            ++i;
            digits = true;
        }
    }
    if (!digits) return false;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        bool exponent = false;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) != 0) {
            ++i;
            exponent = true;
        }
        if (!exponent) return false;
    }
    return i == s.size();
}

// plain 标量是否安全:出任何岔子(空串、保留词、数字样子、引导指示符、
// 首尾空白、": "、" #"、控制字节)都返回 false,让调用方加双引号。
bool PlainSafe(const std::string& s) {
    if (s.empty() || LooksLikeSpecialWord(s) || LooksLikeNumber(s)) return false;
    static constexpr const char* indicators = "-?:,[]{}#&*!|>'\"%@`";
    if (std::strchr(indicators, s.front()) != nullptr) return false;
    if (s.front() == ' ' || s.back() == ' ' || s.back() == ':') return false;
    if (s.find(": ") != std::string::npos) return false;
    if (s.find(" #") != std::string::npos) return false;
    return std::none_of(s.begin(), s.end(), IsControlByte);
}

void EmitScalar(YAML::Emitter& out, const std::string& value) {
    if (PlainSafe(value)) {
        out << value;
    } else {
        out << YAML::DoubleQuoted << value;
    }
}

void BeginSeq(YAML::Emitter& out, bool empty) {
    // 空 seq 走 flow 才有 "[]" 的定型写法;非空走 block(每项一行 "- ")。
    if (empty) out << YAML::Flow;
    out << YAML::BeginSeq;
}

void BeginMap(YAML::Emitter& out, bool empty) {
    if (empty) out << YAML::Flow;
    out << YAML::BeginMap;
}

// evidence 写出清单:老条目的独立 paths 并进证据(schema 3 无 paths 字段),
// 已在 evidence 里的路径不重复。
std::vector<MemoryEvidence> MergedEvidence(const MemoryEntry& entry) {
    std::vector<MemoryEvidence> merged = entry.evidence;
    std::vector<std::string> seen;
    seen.reserve(merged.size());
    for (const auto& item : merged) seen.push_back(item.path);
    for (const std::string& path : entry.paths) {
        if (std::find(seen.begin(), seen.end(), path) == seen.end()) {
            merged.push_back(MemoryEvidence{path, std::string()});
        }
    }
    return merged;
}

// ---- parse 侧 ----

std::string ScalarText(const YAML::Node& node) {
    if (!node.IsDefined() || !node.IsScalar()) return {};
    return node.Scalar();
}

// 单独一行 --- 判定(容忍行尾 \r)。返回这行在全文里的字节区间。
std::size_t FindDelimiterLine(const std::string& text, std::size_t from) {
    std::size_t line_begin = from;
    while (line_begin < text.size()) {
        const std::size_t line_end = text.find('\n', line_begin);
        const std::size_t content_end = (line_end == std::string::npos ? text.size() : line_end) -
                                        (line_end != std::string::npos && line_end > line_begin &&
                                                 text[line_end - 1] == '\r'
                                             ? 1
                                             : 0);
        if (text.compare(line_begin, content_end - line_begin, "---") == 0) {
            return line_begin;
        }
        if (line_end == std::string::npos) break;
        line_begin = line_end + 1;
    }
    return std::string::npos;
}

// 正文里抽标题并把它从正文里剥掉:首个非空行是一级标题(# 开头)就用它,
// body 返回去头后的正文;没有标题就退回 name/name 为空再退 id,body 原样。
// 标题截到单行,别把多行标题塞进 MemoryEntry。
std::string SplitTitleFromBody(const std::string& body, const MemoryEntry& entry, std::string& body_out) {
    std::size_t pos = 0;
    while (pos < body.size()) {
        const std::size_t line_end = body.find('\n', pos);
        const std::string line =
            body.substr(pos, (line_end == std::string::npos ? body.size() : line_end) - pos);
        const std::string trimmed = TrimWhitespace(line);
        if (!trimmed.empty()) {
            if (trimmed.starts_with("# ")) {
                body_out = TrimWhitespace(body.substr((line_end == std::string::npos ? body.size() : line_end) + 1));
                return OneLine(trimmed.substr(2), kMaxTitleBytes);
            }
            break;  // 首个非空行不是一级标题:不再往下找,退回缺省
        }
        if (line_end == std::string::npos) break;
        pos = line_end + 1;
    }
    body_out = TrimWhitespace(body);
    return entry.name.empty() ? entry.id : entry.name;
}

}  // namespace

std::expected<ParsedTopic, std::string> Parse(const std::string& text) {
    // 只认文件开头第一对 ---:首行必须是单独一行 ---,结束符是其后第一条
    // 单独的 --- 线;正文里的水平线不影响(扫描在第一条就停了)。
    if (FindDelimiterLine(text, 0) != 0) {
        return std::unexpected("缺 front matter 起始分隔线");
    }
    const std::size_t first_line_end = text.find('\n');
    if (first_line_end == std::string::npos) {
        return std::unexpected("front matter 没有闭合分隔线");
    }
    const std::size_t close = FindDelimiterLine(text, first_line_end + 1);
    if (close == std::string::npos) {
        return std::unexpected("front matter 没有闭合分隔线");
    }
    YAML::Node root;
    try {
        root = YAML::Load(text.substr(0, close));
    } catch (const YAML::Exception& e) {
        return std::unexpected(std::string("front matter 不是合法 YAML: ") + e.what());
    }
    if (!root.IsMap()) return std::unexpected("front matter 顶层须是映射");
    const YAML::Node meta = root["metadata"];
    if (!meta.IsDefined() || !meta.IsMap()) return std::unexpected("front matter 缺 metadata 映射");
    int schema = 0;
    if (meta["schema"].IsDefined()) {
        try {
            schema = meta["schema"].as<int>();
        } catch (const YAML::Exception&) {
            schema = 0;
        }
    }
    if (schema != 3) return std::unexpected("metadata.schema 只认 3");

    ParsedTopic topic;
    MemoryEntry& entry = topic.entry;
    entry.schema = 3;
    entry.name = ScalarText(root["name"]);
    entry.summary = ScalarText(root["description"]);
    const std::string node_type = ScalarText(meta["node_type"]);
    if (!node_type.empty() && node_type != "memory") {
        return std::unexpected("metadata.node_type 只认 memory,留作他日后扩展");
    }
    auto kind = ParseMemoryKind(ScalarText(meta["type"]));
    if (!kind.has_value()) return std::unexpected(kind.error());
    entry.kind = *kind;
    entry.id = ScalarText(meta["id"]);
    entry.confidence = ScalarText(meta["confidence"]);
    if (entry.confidence.empty()) {
        entry.confidence = entry.kind == MemoryKind::Fact ? "verified" : "user-stated";
    }
    entry.status = ScalarText(meta["status"]);
    if (entry.status.empty()) entry.status = "active";
    const YAML::Node scope = meta["scope"];
    if (scope.IsDefined()) {
        if (!scope.IsMap()) return std::unexpected("metadata.scope 须是映射");
        entry.scope.level = ScalarText(scope["level"]);
        entry.scope.kind = ScalarText(scope["kind"]);
        entry.scope.value = ScalarText(scope["value"]);
    }
    if (entry.scope.level.empty()) entry.scope.level = "project";
    if (entry.scope.kind.empty()) entry.scope.kind = "project";
    if (const YAML::Node sessions = meta["origin_session_ids"]; sessions.IsDefined()) {
        if (!sessions.IsSequence()) return std::unexpected("metadata.origin_session_ids 须是序列");
        for (const auto& item : sessions) {
            const std::string session = ScalarText(item);
            if (!session.empty()) entry.source_sessions.push_back(session);
        }
    }
    entry.created_at = ScalarText(meta["created"]);
    entry.updated_at = ScalarText(meta["modified"]);
    if (entry.created_at.empty()) entry.created_at = entry.updated_at;
    entry.last_verified_at = ScalarText(meta["last_verified"]);
    const YAML::Node expires = meta["expires"];
    if (expires.IsDefined() && !expires.IsNull()) {
        entry.expires_at = ScalarText(expires);
    }
    // 时间线锚点:旧条目无 occurred_at 读入为空(不参与时间排序),形状
    // 不像日期也按空处理——不造假。
    const YAML::Node occurred = meta["occurred_at"];
    if (occurred.IsDefined() && !occurred.IsNull()) {
        const std::string occurred_text = ScalarText(occurred);
        if (LooksLikeMemoryDate(occurred_text)) entry.occurred_at = occurred_text;
    }
    if (const YAML::Node keywords = meta["keywords"]; keywords.IsDefined()) {
        if (!keywords.IsSequence()) return std::unexpected("metadata.keywords 须是序列");
        for (const auto& item : keywords) {
            const std::string keyword = ScalarText(item);
            if (!keyword.empty()) entry.keywords.push_back(keyword);
        }
    }
    if (const YAML::Node evidence = meta["evidence"]; evidence.IsDefined()) {
        if (!evidence.IsSequence()) return std::unexpected("metadata.evidence 须是序列");
        for (const auto& item : evidence) {
            if (!item.IsMap()) continue;
            MemoryEvidence proof;
            proof.path = ScalarText(item["path"]);
            proof.symbol = ScalarText(item["symbol"]);
            if (!proof.path.empty()) entry.evidence.push_back(std::move(proof));
        }
    }
    // schema 3 无独立 paths 字段:证据路径即硬命中路径。
    entry.paths.clear();
    for (const auto& item : entry.evidence) entry.paths.push_back(item.path);
    if (const YAML::Node fingerprints = meta["fingerprints"]; fingerprints.IsDefined() && fingerprints.IsMap()) {
        for (auto it = fingerprints.begin(); it != fingerprints.end(); ++it) {
            if (!it->first.IsScalar() || !it->second.IsScalar()) continue;
            topic.fingerprints[it->first.Scalar()] = it->second.Scalar();
        }
    }
    const std::size_t body_begin = text.find('\n', close);
    const std::string raw_body =
        body_begin == std::string::npos ? std::string() : text.substr(body_begin + 1);
    entry.title = SplitTitleFromBody(raw_body, entry, topic.body);
    return topic;
}

std::string BuildTopicText(const MemoryEntry& entry, const nlohmann::json& fingerprints,
                           const std::string& body) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "name" << YAML::Value;
    EmitScalar(out, entry.name);
    out << YAML::Key << "description" << YAML::Value;
    EmitScalar(out, entry.summary);
    out << YAML::Key << "metadata" << YAML::Value;
    BeginMap(out, false);
    out << YAML::Key << "schema" << YAML::Value << 3;
    out << YAML::Key << "node_type" << YAML::Value;
    EmitScalar(out, "memory");
    out << YAML::Key << "type" << YAML::Value;
    EmitScalar(out, MemoryKindName(entry.kind));
    out << YAML::Key << "id" << YAML::Value;
    EmitScalar(out, entry.id);
    out << YAML::Key << "confidence" << YAML::Value;
    EmitScalar(out, entry.confidence);
    out << YAML::Key << "status" << YAML::Value;
    EmitScalar(out, entry.status);
    out << YAML::Key << "scope" << YAML::Value;
    BeginMap(out, false);
    out << YAML::Key << "level" << YAML::Value;
    EmitScalar(out, entry.scope.level.empty() ? std::string("project") : entry.scope.level);
    out << YAML::Key << "kind" << YAML::Value;
    EmitScalar(out, entry.scope.kind.empty() ? std::string("project") : entry.scope.kind);
    out << YAML::Key << "value" << YAML::Value;
    EmitScalar(out, entry.scope.value);
    out << YAML::EndMap;
    out << YAML::Key << "origin_session_ids" << YAML::Value;
    BeginSeq(out, entry.source_sessions.empty());
    for (const std::string& session : entry.source_sessions) EmitScalar(out, session);
    out << YAML::EndSeq;
    out << YAML::Key << "created" << YAML::Value;
    EmitScalar(out, entry.created_at);
    out << YAML::Key << "modified" << YAML::Value;
    EmitScalar(out, entry.updated_at);
    out << YAML::Key << "last_verified" << YAML::Value;
    EmitScalar(out, entry.last_verified_at);
    out << YAML::Key << "expires" << YAML::Value;
    if (entry.expires_at.empty()) {
        out << YAML::Null;
    } else {
        EmitScalar(out, entry.expires_at);
    }
    // 时间线锚点:空 = null(与 expires 同款);非空由写入侧校过形状。
    out << YAML::Key << "occurred_at" << YAML::Value;
    if (entry.occurred_at.empty()) {
        out << YAML::Null;
    } else {
        EmitScalar(out, entry.occurred_at);
    }
    out << YAML::Key << "keywords" << YAML::Value;
    BeginSeq(out, entry.keywords.empty());
    for (const std::string& keyword : entry.keywords) EmitScalar(out, keyword);
    out << YAML::EndSeq;
    const std::vector<MemoryEvidence> evidence = MergedEvidence(entry);
    out << YAML::Key << "evidence" << YAML::Value;
    BeginSeq(out, evidence.empty());
    for (const MemoryEvidence& item : evidence) {
        BeginMap(out, false);
        out << YAML::Key << "path" << YAML::Value;
        EmitScalar(out, item.path);
        out << YAML::Key << "symbol" << YAML::Value;
        EmitScalar(out, item.symbol);
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;
    const bool has_fingerprints =
        fingerprints.is_object() && !fingerprints.empty() &&
        std::all_of(fingerprints.begin(), fingerprints.end(),
                    [](const auto& item) { return item.is_string(); });
    out << YAML::Key << "fingerprints" << YAML::Value;
    BeginMap(out, !has_fingerprints);
    if (has_fingerprints) {
        for (auto it = fingerprints.begin(); it != fingerprints.end(); ++it) {
            out << YAML::Key;
            EmitScalar(out, it.key());
            out << YAML::Value;
            EmitScalar(out, it.value().get<std::string>());
        }
    }
    out << YAML::EndMap;
    out << YAML::EndMap;  // metadata
    out << YAML::EndMap;  // 根映射

    std::string text = "---\n";
    text += out.c_str();
    text += "\n---\n\n# ";
    text += OneLine(entry.title, kMaxTitleBytes);
    text += "\n\n";
    text += TrimWhitespace(body);
    text += "\n";
    return text;
}

std::string StripTitleHeading(const std::string& body) {
    std::size_t pos = 0;
    while (pos < body.size()) {
        const std::size_t line_end = body.find('\n', pos);
        const std::string line =
            body.substr(pos, (line_end == std::string::npos ? body.size() : line_end) - pos);
        const std::string trimmed = TrimWhitespace(line);
        if (!trimmed.empty()) {
            if (trimmed.starts_with("# ")) {
                return TrimWhitespace(
                    body.substr((line_end == std::string::npos ? body.size() : line_end) + 1));
            }
            return TrimWhitespace(body);
        }
        if (line_end == std::string::npos) return std::string();
        pos = line_end + 1;
    }
    return std::string();
}

std::string StripTopicMetadata(const std::string& text) {
    if (text.starts_with("---\n") || text.starts_with("---\r\n")) {
        const std::size_t first_line_end = text.find('\n');
        const std::size_t close = FindDelimiterLine(text, first_line_end + 1);
        if (close == std::string::npos) return text;
        const std::size_t body_begin = text.find('\n', close);
        return body_begin == std::string::npos
                   ? std::string()
                   : TrimWhitespace(text.substr(body_begin + 1));
    }
    if (text.starts_with(kLegacyMetaOpen)) {
        const std::size_t end = text.find(kLegacyMetaClose, kLegacyMetaOpen.size());
        if (end == std::string::npos) return text;
        return TrimWhitespace(text.substr(end + kLegacyMetaClose.size()));
    }
    return text;
}

}  // namespace lubancode::memory::frontmatter
