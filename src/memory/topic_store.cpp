// topic_store.hpp 的实现。SV-09(2026-09-21 架构审查)拆分前全住
// project_memory.cpp,搬来逻辑一字未动;ProjectMemory::FindTopic 改名
// FindTopicAcrossLayers(跨层找主题),PlanMigration/RunMigration 的
// 实现体改名 PlanSchemaMigration/RunSchemaMigration,目录由参数递入。

#include "memory/topic_store.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>

#include "hooks/hash.hpp"         // Sha256Hex:upsert 回执的正文指纹
#include "memory/frontmatter.hpp"
#include "memory/internal.hpp"
#include "memory/recall_engine.hpp"  // recall::BuildContentIndexBag:catalog 词袋

namespace lubancode::memory {

namespace {

namespace fs = std::filesystem;

// 旧格式(schema 1/2)的元数据标记,常量真本在 frontmatter.hpp 共用。
constexpr std::string_view kMetaOpen = frontmatter::kLegacyMetaOpen;
constexpr std::string_view kMetaClose = frontmatter::kLegacyMetaClose;

nlohmann::json EntryMetadata(const store::StoredEntry& entry) {
    nlohmann::json evidence = nlohmann::json::array();
    for (const auto& item : entry.public_entry.evidence) {
        evidence.push_back(nlohmann::json{{"path", item.path}, {"symbol", item.symbol}});
    }
    return nlohmann::json{
        {"schema", entry.public_entry.schema},
        {"id", entry.public_entry.id},
        {"name", entry.public_entry.name},
        {"kind", MemoryKindName(entry.public_entry.kind)},
        {"title", entry.public_entry.title},
        {"summary", entry.public_entry.summary},
        {"keywords", entry.public_entry.keywords},
        {"paths", entry.public_entry.paths},
        {"status", entry.public_entry.status},
        {"updated_at", entry.public_entry.updated_at},
        {"created_at", entry.public_entry.created_at},
        {"source_sessions", entry.public_entry.source_sessions},
        {"scope", nlohmann::json{{"level", entry.public_entry.scope.level},
                                 {"kind", entry.public_entry.scope.kind},
                                 {"value", entry.public_entry.scope.value}}},
        {"evidence", evidence},
        {"confidence", entry.public_entry.confidence},
        {"last_verified_at", entry.public_entry.last_verified_at},
        {"expires_at", entry.public_entry.expires_at.empty() ? nlohmann::json()
                                                             : nlohmann::json(entry.public_entry.expires_at)},
        {"occurred_at", entry.public_entry.occurred_at.empty()
                            ? nlohmann::json()
                            : nlohmann::json(entry.public_entry.occurred_at)},
        {"fingerprints", entry.fingerprints},
        // content 进索引:词袋随 catalog 落盘,生产检索每轮只解析词袋,不重
        // 切全文(旧 catalog 无此键,读回为空,退回无正文索引,下次 rebuild
        // 自然补上)。
        {"content_index", recall::BuildContentIndexBag(entry.public_entry.content)},
    };
}

std::expected<store::StoredEntry, std::string> ParseStoredEntry(const nlohmann::json& meta,
                                                                const std::string& relative_file) {
    // schema 1/2 平滑迁移:老主题照读,新字段填缺省值(confidence 按 kind 推
    // 定,scope=project);下次同 id 保存或核验时自然写成 schema 3,老正文
    // 一字不动。schema 3 是 front matter 主题;catalog 里存的是同一份内部
    // 结构(带 name/created_at),字段对齐读。
    const int schema = meta.value("schema", 0);
    if (!meta.is_object() || (schema != 1 && schema != 2 && schema != 3)) {
        return std::unexpected("记忆元数据 schema 不受支持");
    }
    store::StoredEntry entry;
    entry.public_entry.schema = schema;
    entry.public_entry.id = meta.value("id", std::string());
    entry.public_entry.name = meta.value("name", std::string());
    entry.public_entry.title = meta.value("title", std::string());
    entry.public_entry.summary = meta.value("summary", std::string());
    entry.public_entry.file = relative_file;
    entry.public_entry.status = meta.value("status", std::string("active"));
    entry.public_entry.updated_at = meta.value("updated_at", std::string());
    entry.public_entry.created_at = meta.value("created_at", std::string());
    if (entry.public_entry.created_at.empty()) entry.public_entry.created_at = entry.public_entry.updated_at;
    if (!IsSafeRelativePath(relative_file)) {
        return std::unexpected("记忆文件路径越出 memory 根");
    }
    auto kind = ParseMemoryKind(meta.value("kind", std::string()));
    if (!kind.has_value()) {
        return std::unexpected(kind.error());
    }
    entry.public_entry.kind = *kind;
    if (!IsValidId(entry.public_entry.id) || entry.public_entry.title.empty()) {
        return std::unexpected("记忆元数据缺 id 或 title");
    }
    if (meta.contains("keywords") && meta["keywords"].is_array()) {
        for (const auto& item : meta["keywords"]) {
            if (item.is_string()) entry.public_entry.keywords.push_back(item.get<std::string>());
        }
    }
    if (meta.contains("paths") && meta["paths"].is_array()) {
        for (const auto& item : meta["paths"]) {
            if (item.is_string() && IsSafeRelativePath(item.get<std::string>())) {
                entry.public_entry.paths.push_back(item.get<std::string>());
            }
        }
    }
    if (meta.contains("source_sessions") && meta["source_sessions"].is_array()) {
        for (const auto& item : meta["source_sessions"]) {
            if (item.is_string()) entry.public_entry.source_sessions.push_back(item.get<std::string>());
        }
    }
    if (meta.contains("scope") && meta["scope"].is_object()) {
        entry.public_entry.scope.level = meta["scope"].value("level", std::string("project"));
        entry.public_entry.scope.kind = meta["scope"].value("kind", std::string("project"));
        entry.public_entry.scope.value = meta["scope"].value("value", std::string());
    }
    if (meta.contains("evidence") && meta["evidence"].is_array()) {
        for (const auto& item : meta["evidence"]) {
            if (!item.is_object()) continue;
            MemoryEvidence evidence;
            evidence.path = item.value("path", std::string());
            evidence.symbol = item.value("symbol", std::string());
            if (!evidence.path.empty()) entry.public_entry.evidence.push_back(std::move(evidence));
        }
    }
    entry.public_entry.confidence = meta.value("confidence", std::string());
    if (entry.public_entry.confidence.empty()) {
        // schema 1 缺省推定:fact 本就该核验过,preference 本就该用户明说。
        entry.public_entry.confidence =
            entry.public_entry.kind == MemoryKind::Fact ? "verified" : "user-stated";
    }
    entry.public_entry.last_verified_at = meta.value("last_verified_at", std::string());
    if (meta.contains("expires_at") && meta["expires_at"].is_string()) {
        entry.public_entry.expires_at = meta["expires_at"].get<std::string>();
    }
    // 时间线锚点:旧 catalog 无此键读空;形状不像日期按空(不造假)。
    if (meta.contains("occurred_at") && meta["occurred_at"].is_string()) {
        const std::string occurred = meta["occurred_at"].get<std::string>();
        if (LooksLikeMemoryDate(occurred)) entry.public_entry.occurred_at = occurred;
    }
    if (meta.contains("fingerprints") && meta["fingerprints"].is_object()) {
        entry.fingerprints = meta["fingerprints"];
    }
    // content 进索引:catalog 里的预分词正文词袋(旧档无此键,空串=该层
    // 还没 rebuild 过,检索自然退回无正文索引)。
    entry.public_entry.content_index = meta.value("content_index", std::string());
    return entry;
}

std::expected<store::StoredEntry, std::string> ParseTopicFile(const fs::path& path,
                                                              const fs::path& memory_dir,
                                                              const char* layer) {
    const std::string text = ReadFile(path);
    std::error_code ec;
    const fs::path relative = fs::relative(path, memory_dir, ec);
    const std::string relative_file = ec ? PathUtf8(path.filename()) : PathUtf8(relative);
    // 双格式 reader:schema 3 走 front matter(YAML),schema 1/2 走 HTML
    // 注释里的严格 JSON。新写一律 schema 3,旧主题照读照召回。
    if (text.starts_with("---\n") || text.starts_with("---\r\n")) {
        auto parsed = frontmatter::Parse(text);
        if (!parsed.has_value()) return std::unexpected(parsed.error());
        store::StoredEntry stored;
        stored.public_entry = std::move(parsed->entry);
        stored.fingerprints = std::move(parsed->fingerprints);
        // content 进索引:正文本体(标题行已由 front matter 层剥掉)随条目
        // 进内存——catalog 重建时抽词成袋,检索兜底路(无 catalog)现切。
        stored.public_entry.content = std::move(parsed->body);
        stored.public_entry.file = relative_file;
        if (!IsSafeRelativePath(relative_file)) {
            return std::unexpected("记忆文件路径越出 memory 根");
        }
        if (!IsValidId(stored.public_entry.id)) {
            return std::unexpected("记忆元数据缺 id 或 id 不合法");
        }
        for (const std::string& item : stored.public_entry.paths) {
            if (!IsSafeRelativePath(item)) {
                return std::unexpected("记忆 paths 只许项目内相对路径: " + item);
            }
        }
        if (stored.public_entry.scope.level != layer) {
            return std::unexpected(std::string("scope.level 与所在目录层不符(应为 ") + layer + ")");
        }
        return stored;
    }
    if (!text.starts_with(kMetaOpen)) {
        return std::unexpected("缺 lubancode-memory 元数据或 front matter");
    }
    const std::size_t end = text.find(kMetaClose, kMetaOpen.size());
    if (end == std::string::npos) {
        return std::unexpected("记忆元数据没有闭合");
    }
    nlohmann::json meta;
    try {
        meta = nlohmann::json::parse(text.substr(kMetaOpen.size(), end - kMetaOpen.size()));
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected("记忆元数据不是合法 JSON: " + std::string(e.what()));
    }
    auto stored = ParseStoredEntry(meta, relative_file);
    if (stored.has_value()) {
        // 旧格式同款:正文剥掉元数据头与标题行再进 content。
        stored->public_entry.content =
            frontmatter::StripTitleHeading(frontmatter::StripTopicMetadata(text));
    }
    return stored;
}

std::string EscapeMarkdownLabel(std::string value) {
    value = OneLine(std::move(value), 160);
    for (char& c : value) {
        if (c == '[' || c == ']') c = ' ';
    }
    return value;
}

std::string BuildIndex(const std::vector<store::StoredEntry>& entries, const char* layer) {
    std::ostringstream out;
    out << (layer == std::string_view("user") ? "# User Memory\n\n" : "# Project Memory\n\n")
        << "<!-- 此文件由 LubanCode 生成。请改主题文件，不要直接改索引。 -->\n\n";
    const bool user_layer = layer == std::string_view("user");
    for (const auto& [kind, heading] : {std::pair{MemoryKind::Fact, "Facts"},
                                       std::pair{MemoryKind::Preference, "Preferences"},
                                       std::pair{MemoryKind::Feedback, "Feedback"}}) {
        if (user_layer && kind == MemoryKind::Fact) continue;  // 用户层不放事实
        out << "## " << heading << "\n\n";
        bool any = false;
        for (const auto& stored : entries) {
            const MemoryEntry& entry = stored.public_entry;
            if (entry.kind != kind || entry.status == "archived") continue;
            any = true;
            out << "- [" << EscapeMarkdownLabel(entry.title) << "](" << entry.file << ") — "
                << OneLine(entry.summary.empty() ? entry.title : entry.summary)
                << "；id: `" << entry.id << "`";
            if (entry.status != "active") out << "；status: `" << entry.status << "`";
            out << "\n";
        }
        if (!any) out << "- (empty)\n";
        out << "\n";
    }
    return out.str();
}

std::string FileFingerprint(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return {};
    std::uint64_t hash = 14695981039346656037ULL;
    char buffer[8192];
    while (file.good()) {
        file.read(buffer, sizeof(buffer));
        const std::streamsize count = file.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ULL;
        }
    }
    std::ostringstream out;
    out << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

// ---- 时间线锚点(记忆写入侧改进单) ----
// 正文头部的人类可读时间锚:单行【YYYY-MM-DD…】。全库一致由这一处出;
// 重写时先剥旧锚再按 occurred_at 补新锚,日期不变不翻倍,occurred_at 空
// 则只剥不补(不造假)。

bool IsTimeAnchorLine(const std::string& line) {
    const std::string trimmed = Trim(line);
    if (trimmed.size() < 12 || trimmed.front() != '\xe3') return false;  // 【 是三字节 UTF-8
    // 找配对的闭括号】:正文里以【开头的句子不少,只有"整行恰是【…】且
    // 内文是日期形状"才算锚。
    const std::size_t open_bytes = 3;   // 【
    const std::size_t close_bytes = 3;  // 】
    if (trimmed.size() < open_bytes + close_bytes + 10) return false;
    const std::size_t close = trimmed.size() - close_bytes;
    if (trimmed.compare(close, close_bytes, "\xe3\x80\x91") != 0) return false;
    const std::string inner = trimmed.substr(open_bytes, close - open_bytes);
    return LooksLikeMemoryDate(inner);
}

std::string ApplyTimeAnchor(const std::string& body, const std::string& occurred_at) {
    // 剥头部旧锚(连同其后空行),正文其余一字不动。
    std::string rest = body;
    while (true) {
        const std::size_t eol = rest.find('\n');
        const std::string line = eol == std::string::npos ? rest : rest.substr(0, eol);
        if (IsTimeAnchorLine(line)) {
            rest = eol == std::string::npos ? std::string() : rest.substr(eol + 1);
            continue;
        }
        if (Trim(line).empty() && eol != std::string::npos) {
            // 锚后面的空行顺手吃掉;正文开头的空行原本也会被 Trim 掉。
            rest = rest.substr(eol + 1);
            continue;
        }
        break;
    }
    rest = Trim(rest);
    if (occurred_at.empty()) return rest;
    return "\xe3\x80\x90" + occurred_at + "\xe3\x80\x91\n\n" + rest;
}

bool LooksSensitive(const SaveRequest& request) {
    std::string haystack = LowerAscii(request.title + "\n" + request.summary + "\n" + request.content);
    static constexpr std::string_view patterns[] = {
        "-----begin private key", "api_key=", "apikey=", "authorization: bearer ",
        "password=", "passwd=", "cookie:", "secret=", "sk-proj-", "sk-ant-"};
    for (const auto pattern : patterns) {
        if (haystack.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::expected<void, std::string> ValidateScope(const MemoryScope& scope) {
    if (scope.level == "user") {
        // 用户级记忆(跨项目偏好/反馈):不放仓库事实,不假借项目路径作
        // 证据——证据清一色要在写入处再拦一道,这里先守 scope 自身齐整。
        if (scope.kind != "user") {
            return std::unexpected("scope.level=user 时 kind 须为 user");
        }
        if (!scope.value.empty()) {
            return std::unexpected("用户级记忆不带 scope.value(它不属于任何项目)");
        }
        return {};
    }
    if (scope.level != "project" && !scope.level.empty()) {
        return std::unexpected("scope.level 只认 project 或 user");
    }
    if (scope.kind == "user") {
        return std::unexpected("scope.kind=user 须配 level=user");
    }
    if (scope.kind == "project") return {};
    if (scope.kind == "global") {
        // 跨项目/全局经验按用户层分账:要走 level=user 那条路,不认旧键。
        return std::unexpected("scope=global 不再单独开放,跨项目经验走 level=user");
    }
    if (scope.kind != "subtree" && scope.kind != "path") {
        return std::unexpected("scope.kind 只认 project、subtree、path 或 user");
    }
    if (!IsSafeRelativePath(scope.value)) {
        return std::unexpected("scope=subtree/path 须带项目内相对路径");
    }
    return {};
}

std::string BuildTopicText(const store::StoredEntry& entry, const std::string& content) {
    return frontmatter::BuildTopicText(entry.public_entry, entry.fingerprints, content);
}

// id 去类型前缀得 name(schema 3 的文件 slug)。id 本就验证过字符集,这里
// 只做切分与兜底。
std::string NameFromId(const std::string& id, const std::string& kind_name) {
    const std::string prefix = kind_name + ".";
    if (id.starts_with(prefix) && id.size() > prefix.size()) {
        return id.substr(prefix.size());
    }
    return id;
}

// canonical 路径:schema 3 一律住 <类型目录>/<name>.md。
const char* KindFolder(MemoryKind kind) {
    switch (kind) {
        case MemoryKind::Fact: return "facts";
        case MemoryKind::Preference: return "preferences";
        case MemoryKind::Feedback: return "feedback";
    }
    return "facts";
}

std::string CanonicalTopicFile(MemoryKind kind, const std::string& name) {
    return std::string(KindFolder(kind)) + "/" + name + ".md";
}

std::string Slug(std::string value) {
    value = LowerAscii(std::move(value));
    std::string out;
    bool dash = false;
    for (const unsigned char byte : value) {
        if (std::isalnum(byte) != 0) {
            out.push_back(static_cast<char>(byte));
            dash = false;
        } else if ((byte == '-' || byte == '_' || byte == '.' || std::isspace(byte) != 0) && !out.empty() && !dash) {
            out.push_back('-');
            dash = true;
        }
        if (out.size() >= 80) {
            break;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    if (out.empty()) {
        out = "memory-" + HexHash(value).substr(0, 12);
    }
    return out;
}

}  // namespace

namespace store {

std::vector<StoredEntry> ScanTopics(const fs::path& memory_dir, std::vector<std::string>* warnings,
                                    const char* layer) {
    std::vector<StoredEntry> entries;
    // 用户层不放 facts:跨项目只有偏好与反馈,仓库事实住项目层。
    const std::vector<const char*> folders = layer == std::string_view("user")
                                                 ? std::vector<const char*>{"preferences", "feedback"}
                                                 : std::vector<const char*>{"facts", "preferences",
                                                                            "feedback"};
    for (const char* folder : folders) {
        const fs::path root = memory_dir / folder;
        std::error_code ec;
        fs::directory_iterator it(root, ec);
        if (ec) {
            continue;
        }
        for (const auto& item : it) {
            if (!item.is_regular_file(ec) || item.path().extension() != ".md") {
                continue;
            }
            auto parsed = ParseTopicFile(item.path(), memory_dir, layer);
            if (parsed.has_value()) {
                entries.push_back(std::move(*parsed));
            } else if (warnings != nullptr) {
                warnings->push_back(PathUtf8(item.path()) + ": " + parsed.error());
            }
        }
    }
    // 同 id 撞车:两份都停成 conflict,不凭时间偷偷选一份。重建 catalog、
    // list、召回全认这个状态(conflict 不注入)。
    std::unordered_map<std::string, std::vector<std::size_t>> by_id;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        by_id[entries[i].public_entry.id].push_back(i);
    }
    for (auto& [id, indexes] : by_id) {
        if (indexes.size() < 2) continue;
        for (const std::size_t index : indexes) {
            entries[index].public_entry.status = "conflict";
        }
        if (warnings != nullptr) {
            std::string files;
            for (const std::size_t index : indexes) {
                if (!files.empty()) files += ", ";
                files += entries[index].public_entry.file;
            }
            warnings->push_back("两份主题撞同一 id " + id + ": " + files + ";已标 conflict,须手工处置");
        }
    }
    std::sort(entries.begin(), entries.end(), [](const StoredEntry& a, const StoredEntry& b) {
        if (a.public_entry.kind != b.public_entry.kind) {
            return a.public_entry.kind < b.public_entry.kind;
        }
        return a.public_entry.id < b.public_entry.id;
    });
    return entries;
}

std::vector<StoredEntry> LoadCatalog(const fs::path& memory_dir, std::string* error,
                                     const char* layer) {
    const fs::path path = memory_dir / ".state" / "catalog.json";
    const std::string text = ReadFile(path);
    if (text.empty()) {
        return ScanTopics(memory_dir, nullptr, layer);
    }
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        if (error != nullptr) *error = e.what();
        return ScanTopics(memory_dir, nullptr, layer);
    }
    if (!root.is_object() || !root.contains("entries") || !root["entries"].is_array()) {
        if (error != nullptr) *error = "catalog 结构不对";
        return ScanTopics(memory_dir, nullptr, layer);
    }
    std::vector<StoredEntry> entries;
    for (const auto& item : root["entries"]) {
        const std::string file = item.value("file", std::string());
        auto parsed = ParseStoredEntry(item, file);
        if (parsed.has_value()) entries.push_back(std::move(*parsed));
    }
    return entries;
}

bool FingerprintsCurrent(const StoredEntry& entry, const fs::path& project_root) {
    if (!entry.fingerprints.is_object()) return true;
    for (auto it = entry.fingerprints.begin(); it != entry.fingerprints.end(); ++it) {
        if (!it.value().is_string() || !IsSafeRelativePath(it.key())) continue;
        const std::string actual = FileFingerprint(project_root / Utf8Path(it.key()));
        if (actual.empty() || actual != it.value().get<std::string>()) return false;
    }
    return true;
}

std::expected<void, std::string> ValidateSaveRequest(const SaveRequest& request) {
    if (request.title.empty() || request.title.size() > kMaxTitleBytes) {
        return std::unexpected("记忆标题不能为空，且不能超过 200 字节");
    }
    if (request.content.empty() || request.content.size() > kMaxTopicBytes) {
        return std::unexpected("记忆正文不能为空，且不能超过 8 KiB");
    }
    if (request.summary.size() > kMaxSummaryBytes) {
        return std::unexpected("记忆摘要不能超过 500 字节");
    }
    if (!request.id.empty()) {
        if (!IsValidId(request.id)) {
            return std::unexpected("记忆 id 只许字母、数字、点、短横线、下划线，并须以 fact.、preference. 或 feedback. 开头");
        }
        const std::string want = MemoryKindName(request.kind) + ".";
        if (!request.id.starts_with(want)) {
            return std::unexpected("记忆 id 的前缀与 kind 不符");
        }
    }
    // feedback 只收用户明说的纠正:推断(inferred)不许直写,须先过待审
    // 层或由用户改实。
    if (request.kind == MemoryKind::Feedback && request.confidence == "inferred") {
        return std::unexpected("feedback 只收用户明说的纠正(confidence 须为 user-stated),模型推断不得直写");
    }
    // 用户级记忆只收跨项目偏好/反馈:不放仓库事实,不假借项目路径作证据。
    if (request.scope.level == "user") {
        if (request.kind == MemoryKind::Fact) {
            return std::unexpected("用户级记忆不放仓库事实(fact 只住项目层)");
        }
        if (!request.paths.empty() || !request.evidence.empty()) {
            return std::unexpected("用户级记忆不得假借项目路径作证据,paths/evidence 须为空");
        }
    }
    if (request.keywords.size() > kMaxKeywords || request.paths.size() > kMaxPaths) {
        return std::unexpected("keywords 最多 16 项，paths 最多 24 项");
    }
    for (const std::string& path : request.paths) {
        if (!IsSafeRelativePath(path)) {
            return std::unexpected("记忆 paths 只许项目内相对路径: " + path);
        }
    }
    if (!request.confidence.empty() && request.confidence != "user-stated" &&
        request.confidence != "verified" && request.confidence != "inferred") {
        return std::unexpected("confidence 只认 user-stated、verified 或 inferred");
    }
    if (auto scope = ValidateScope(request.scope); !scope.has_value()) {
        return std::unexpected(scope.error());
    }
    if (request.evidence.size() > kMaxPaths) {
        return std::unexpected("evidence 最多 24 项");
    }
    for (const auto& item : request.evidence) {
        if (!IsSafeRelativePath(item.path)) {
            return std::unexpected("evidence 路径只许项目内相对路径: " + item.path);
        }
    }
    if (!request.expires_at.empty() && !LooksLikeMemoryDate(request.expires_at)) {
        return std::unexpected("expires_at 须是 YYYY-MM-DD 或 ISO 时间");
    }
    // 时间线锚点:形状不对直接拒(工具与 job 正门都过这里);抽取侧的
    // 清洗在 ParseExtractionJson 做,到这的都该是干净日期。
    if (!request.occurred_at.empty() && !LooksLikeMemoryDate(request.occurred_at)) {
        return std::unexpected("occurred_at 须是 YYYY-MM-DD 或 ISO 时间");
    }
    if (LooksSensitive(request)) {
        return std::unexpected("记忆疑似含密钥、口令或认证头，拒绝落盘");
    }
    return {};
}

bool LayerHasEntry(const fs::path& memory_dir, const std::string& id) {
    for (const auto& stored : LoadCatalog(memory_dir, nullptr, "user")) {
        if (stored.public_entry.id == id) return true;
    }
    return false;
}

std::expected<MemoryWriteOutcome, std::string> ProcessUpsert(const nlohmann::json& job,
                                                             const fs::path& memory_dir,
                                                             const fs::path& project_root) {
    SaveRequest request;
    auto kind = ParseMemoryKind(job.value("kind", std::string()));
    if (!kind.has_value()) return std::unexpected(kind.error());
    request.kind = *kind;
    request.id = job.value("id", std::string());
    request.title = job.value("title", std::string());
    request.summary = job.value("summary", std::string());
    request.content = job.value("content", std::string());
    request.source_session = job.value("source_session", std::string());
    if (job.contains("keywords") && job["keywords"].is_array()) {
        for (const auto& item : job["keywords"]) if (item.is_string()) request.keywords.push_back(item);
    }
    if (job.contains("paths") && job["paths"].is_array()) {
        for (const auto& item : job["paths"]) if (item.is_string()) request.paths.push_back(item);
    }
    request.confidence = job.value("confidence", std::string());
    request.expires_at = job.value("expires_at", std::string());
    request.occurred_at = job.value("occurred_at", std::string());
    if (job.contains("scope") && job["scope"].is_object()) {
        request.scope.level = job["scope"].value("level", std::string("project"));
        request.scope.kind = job["scope"].value("kind", std::string("project"));
        request.scope.value = job["scope"].value("value", std::string());
    }
    if (job.contains("evidence") && job["evidence"].is_array()) {
        for (const auto& item : job["evidence"]) {
            if (!item.is_object()) continue;
            MemoryEvidence evidence;
            evidence.path = item.value("path", std::string());
            evidence.symbol = item.value("symbol", std::string());
            if (!evidence.path.empty()) request.evidence.push_back(std::move(evidence));
        }
    }
    if (auto valid = ValidateSaveRequest(request); !valid.has_value()) {
        return std::unexpected(valid.error());
    }

    std::vector<StoredEntry> entries = ScanTopics(memory_dir);
    std::string id = request.id;
    if (id.empty()) id = MemoryKindName(request.kind) + "." + Slug(request.title);

    StoredEntry* existing = nullptr;
    for (auto& entry : entries) {
        if (entry.public_entry.id == id) {
            existing = &entry;
            break;
        }
    }

    StoredEntry updated;
    if (existing != nullptr) updated = *existing;
    const std::string previous_file = updated.public_entry.file;
    updated.public_entry.id = id;
    updated.public_entry.kind = request.kind;
    updated.public_entry.title = OneLine(request.title, kMaxTitleBytes);
    updated.public_entry.summary = OneLine(request.summary.empty() ? request.content : request.summary,
                                           kMaxSummaryBytes);
    updated.public_entry.keywords = request.keywords;
    updated.public_entry.paths = request.paths;
    updated.public_entry.status = "active";
    updated.public_entry.updated_at = NowIsoUtc();
    // 保存即一次核验:盖 last_verified_at。schema 3 新字段一并落定:name 从
    // id 切出来,created_at 保住旧值(老主题用其 updated_at 补)。
    updated.public_entry.last_verified_at = updated.public_entry.updated_at;
    if (updated.public_entry.created_at.empty()) {
        updated.public_entry.created_at = existing != nullptr ? existing->public_entry.updated_at
                                                             : std::string();
        if (updated.public_entry.created_at.empty()) {
            updated.public_entry.created_at = updated.public_entry.updated_at;
        }
    }
    if (!request.confidence.empty()) {
        updated.public_entry.confidence = request.confidence;
    } else {
        updated.public_entry.confidence =
            request.kind == MemoryKind::Fact ? "verified" : "user-stated";
    }
    updated.public_entry.scope = request.scope;
    updated.public_entry.evidence = request.evidence;
    updated.public_entry.expires_at = request.expires_at;
    // 时间线锚点:请求带日期就落;不带保旧值(此前从材料里提过的时间不
    // 因一次没提日期的更新丢掉),不造假也不倒灶。
    if (!request.occurred_at.empty()) {
        updated.public_entry.occurred_at = request.occurred_at;
    }
    if (!request.source_session.empty() &&
        std::find(updated.public_entry.source_sessions.begin(), updated.public_entry.source_sessions.end(),
                  request.source_session) == updated.public_entry.source_sessions.end()) {
        updated.public_entry.source_sessions.push_back(request.source_session);
    }
    updated.public_entry.schema = 3;
    updated.public_entry.name = NameFromId(id, MemoryKindName(request.kind));
    updated.public_entry.file = CanonicalTopicFile(request.kind, updated.public_entry.name);
    // 指纹盖住证据路径 ∪ paths(schema 3 里两者本就该是一份)。
    updated.fingerprints = nlohmann::json::object();
    std::vector<std::string> fingerprint_paths = request.paths;
    for (const MemoryEvidence& proof : request.evidence) {
        if (std::find(fingerprint_paths.begin(), fingerprint_paths.end(), proof.path) ==
            fingerprint_paths.end()) {
            fingerprint_paths.push_back(proof.path);
        }
    }
    for (const std::string& relative : fingerprint_paths) {
        const std::string hash = FileFingerprint(project_root / Utf8Path(relative));
        if (!hash.empty()) updated.fingerprints[relative] = hash;
    }

    const fs::path topic = memory_dir / Utf8Path(updated.public_entry.file);
    auto written = AtomicWrite(topic,
                               BuildTopicText(updated, ApplyTimeAnchor(request.content,
                                                                       updated.public_entry.occurred_at)));
    if (!written.has_value()) return std::unexpected(written.error());
    // 旧文件名不同(老格式或换名)才清;同一把项目锁里先写新再删旧,中途
    // 失败旧文件仍在,新文件不半截落地。
    if (!previous_file.empty() && previous_file != updated.public_entry.file) {
        std::error_code remove_ec;
        fs::remove(memory_dir / Utf8Path(previous_file), remove_ec);
    }
    auto rebuilt = RebuildMemoryIndex(memory_dir, request.scope.level == "user");
    if (!rebuilt.has_value()) return std::unexpected(rebuilt.error());
    MemoryWriteOutcome outcome;
    outcome.memory_id = updated.public_entry.id;
    outcome.memory_path = updated.public_entry.file;
    outcome.content_sha256 = hooks::Sha256Hex(ReadFile(topic));
    outcome.committed_at = updated.public_entry.updated_at;
    return outcome;
}

std::expected<void, std::string> ProcessForget(const nlohmann::json& job, const fs::path& memory_dir) {
    const std::string id = job.value("id", std::string());
    if (!IsValidId(id)) return std::unexpected("forget job 的 id 不合法");
    const bool user_layer = job.value("layer", std::string("project")) == "user";
    const auto entries = ScanTopics(memory_dir, nullptr, user_layer ? "user" : "project");
    for (const auto& entry : entries) {
        if (entry.public_entry.id != id) continue;
        std::error_code ec;
        fs::create_directories(memory_dir / "archive", ec);
        if (ec) return std::unexpected("创建 archive 失败: " + ec.message());
        fs::path destination = memory_dir / "archive" / Utf8Path(entry.public_entry.file).filename();
        if (fs::exists(destination, ec)) {
            destination += "." + JobStamp();
        }
        fs::rename(memory_dir / Utf8Path(entry.public_entry.file), destination, ec);
        if (ec) return std::unexpected("归档记忆失败: " + ec.message());
        return RebuildMemoryIndex(memory_dir, user_layer);
    }
    return std::unexpected("找不到记忆 id: " + id);
}

// 核验(verify/refresh):原 id 复活——正文一字不动,重算指纹、盖
// last_verified_at、status 回 active。这是"核验后续命"那条顺手路径。
std::expected<void, std::string> ProcessVerify(const nlohmann::json& job, const fs::path& memory_dir,
                                               const fs::path& project_root) {
    const std::string id = job.value("id", std::string());
    if (!IsValidId(id)) return std::unexpected("verify job 的 id 不合法");
    const bool refresh = job.value("refresh", false);
    const bool user_layer = job.value("layer", std::string("project")) == "user";
    auto entries = ScanTopics(memory_dir, nullptr, user_layer ? "user" : "project");
    for (auto& stored : entries) {
        if (stored.public_entry.id != id) continue;
        const std::string previous_file = stored.public_entry.file;
        stored.public_entry.last_verified_at = NowIsoUtc();
        if (refresh || stored.public_entry.status != "conflict") {
            stored.public_entry.status = "active";
        }
        stored.fingerprints = nlohmann::json::object();
        for (const std::string& relative : stored.public_entry.paths) {
            const std::string hash = FileFingerprint(project_root / Utf8Path(relative));
            if (!hash.empty()) stored.fingerprints[relative] = hash;
        }
        // 核验顺手升 schema 3:name/created_at 落定,文件挪去规范名。正文一
        // 字不动。
        stored.public_entry.schema = 3;
        stored.public_entry.name = NameFromId(stored.public_entry.id,
                                              MemoryKindName(stored.public_entry.kind));
        if (stored.public_entry.created_at.empty()) {
            stored.public_entry.created_at = stored.public_entry.updated_at;
        }
        stored.public_entry.file = CanonicalTopicFile(stored.public_entry.kind, stored.public_entry.name);
        const std::string content =
            frontmatter::StripTitleHeading(frontmatter::StripTopicMetadata(ReadFile(memory_dir / Utf8Path(previous_file))));
        auto written = AtomicWrite(memory_dir / Utf8Path(stored.public_entry.file),
                                   BuildTopicText(stored, content));
        if (!written.has_value()) return written;
        if (!previous_file.empty() && previous_file != stored.public_entry.file) {
            std::error_code remove_ec;
            fs::remove(memory_dir / Utf8Path(previous_file), remove_ec);
        }
        return RebuildMemoryIndex(memory_dir, user_layer);
    }
    return std::unexpected("找不到记忆 id: " + id);
}

std::optional<std::pair<MemoryEntry, fs::path>> FindTopicAcrossLayers(const fs::path& memory_dir,
                                                                      const fs::path& user_memory_dir,
                                                                      const std::string& id) {
    for (const auto& entry : LoadCatalog(memory_dir)) {
        if (entry.public_entry.id == id) return std::make_pair(entry.public_entry, memory_dir);
    }
    for (const auto& entry : LoadCatalog(user_memory_dir, nullptr, "user")) {
        if (entry.public_entry.id == id) return std::make_pair(entry.public_entry, user_memory_dir);
    }
    return std::nullopt;
}

std::expected<std::pair<std::string, fs::path>, std::string> ReadTopicForShow(
    const fs::path& memory_dir, const fs::path& user_memory_dir, const std::string& id) {
    if (!IsValidId(id)) return std::unexpected("记忆 id 不合法: " + id);
    const auto found = FindTopicAcrossLayers(memory_dir, user_memory_dir, id);
    if (!found.has_value()) return std::unexpected("找不到记忆 id: " + id);
    const auto& [entry, dir] = *found;
    const std::string text = ReadFile(dir / Utf8Path(entry.file));
    if (text.empty()) return std::unexpected("主题文件读不出来: " + entry.file);
    return std::make_pair(text, dir);
}

std::expected<ProjectMemory::TopicEditSession, std::string> BeginTopicEdit(
    const fs::path& memory_dir, const fs::path& user_memory_dir, const std::string& id) {
    auto topic = ReadTopicForShow(memory_dir, user_memory_dir, id);
    if (!topic.has_value()) return std::unexpected(topic.error());
    const auto& [text, dir] = *topic;
    auto parsed = frontmatter::Parse(text);
    if (!parsed.has_value()) {
        return std::unexpected("这份主题不是合法 front matter,先 /memory migrate: " + parsed.error());
    }
    const auto found = FindTopicAcrossLayers(memory_dir, user_memory_dir, id);
    if (!found.has_value()) return std::unexpected("找不到记忆 id: " + id);
    ProjectMemory::TopicEditSession session;
    session.dir = dir;
    session.id = parsed->entry.id;
    session.level = parsed->entry.scope.level;
    session.original = dir / Utf8Path(found->first.file);
    session.scratch = dir / Utf8Path(found->first.file + ".edit-" + JobStamp() + ".md");
    auto staged = AtomicWrite(session.scratch, text);
    if (!staged.has_value()) return std::unexpected(staged.error());
    return session;
}

std::expected<void, std::string> CommitTopicEdit(const ProjectMemory::TopicEditSession& session) {
    const std::string edited = ReadFile(session.scratch);
    std::error_code ec;
    const auto discard = [&session, &ec]() { fs::remove(session.scratch, ec); };
    if (edited.empty()) {
        discard();
        return std::unexpected("编辑后内容为空,原件未动");
    }
    auto parsed = frontmatter::Parse(edited);
    if (!parsed.has_value()) {
        discard();
        return std::unexpected("编辑后的 YAML 不合法,原件未动: " + parsed.error());
    }
    const MemoryEntry& next = parsed->entry;
    // id 与层不许在编辑器里换:换 id 等于造新主题,得走正式写入。
    if (next.id != session.id) {
        discard();
        return std::unexpected("编辑不得改 id(要新建请用 remember/memory_save),原件未动");
    }
    if (next.scope.level != session.level) {
        discard();
        return std::unexpected("编辑不得改 scope.level(跨层搬家走 forget 后重写),原件未动");
    }
    if (!IsValidId(next.id)) {
        discard();
        return std::unexpected("编辑后的 id 不合法,原件未动");
    }
    for (const std::string& path : next.paths) {
        if (!IsSafeRelativePath(path)) {
            discard();
            return std::unexpected("编辑后的 paths 只许项目内相对路径: " + path);
        }
    }
    // 校验通过:同一把项目锁里原子替换,再重建该层派生物。SV-01:锁走
    // 所有权裁决,活持有者在就明说稍后再试,不按年龄抢。
    OwnerLock project_lock;
    const auto lock_result = OwnerLock::TryAcquire(session.dir / ".state" / "memory.lock", &project_lock);
    if (lock_result.status != OwnerLock::Status::Acquired) {
        discard();
        return std::unexpected(ProjectLockRefusal(lock_result, "该层记忆") + ",稍后再试");
    }
    auto replaced = AtomicWrite(session.original, edited);
    discard();
    if (!replaced.has_value()) return std::unexpected(replaced.error());
    return RebuildMemoryIndex(session.dir, session.level == "user");
}

ProjectMemory::MigrationPlan PlanSchemaMigration(const fs::path& memory_dir) {
    std::vector<std::string> warnings;
    const auto entries = ScanTopics(memory_dir, &warnings);
    ProjectMemory::MigrationPlan plan;
    for (const auto& stored : entries) {
        ProjectMemory::MigrationItem item;
        item.file = stored.public_entry.file;
        item.id = stored.public_entry.id;
        if (stored.public_entry.status == "conflict") {
            item.action = "warn";
            item.reason = "与另一份撞同一 id,已停为 conflict,须手工处置";
            ++plan.warnings;
        } else if (stored.public_entry.schema >= 3) {
            item.action = "skip";
            item.reason = "已是 schema 3";
            ++plan.to_skip;
        } else {
            item.action = "migrate";
            item.reason = "schema " + std::to_string(stored.public_entry.schema) + " -> 3";
            ++plan.to_migrate;
        }
        plan.items.push_back(std::move(item));
    }
    // 读不动的坏文件也计进警告(ScanTopics 只往 warnings 里记了路径与原因)。
    for (const std::string& warning : warnings) {
        ProjectMemory::MigrationItem item;
        item.action = "warn";
        item.reason = warning;
        ++plan.warnings;
        plan.items.push_back(std::move(item));
    }
    return plan;
}

std::expected<ProjectMemory::MigrationResult, std::string> RunSchemaMigration(
    const fs::path& memory_dir) {
    ProjectMemory::MigrationPlan plan = PlanSchemaMigration(memory_dir);
    if (plan.to_migrate == 0) {
        return ProjectMemory::MigrationResult{0, std::string()};  // 没活干:重跑不重复
    }

    // 与 worker 同一把项目锁:改名与写新内容须在同一把锁里完成。SV-01:
    // 所有权裁决,不按年龄抢。
    OwnerLock project_lock;
    const auto lock_result = OwnerLock::TryAcquire(memory_dir / ".state" / "memory.lock", &project_lock);
    if (lock_result.status != OwnerLock::Status::Acquired) {
        return std::unexpected(ProjectLockRefusal(lock_result, "项目记忆") + ",稍后再试");
    }

    std::string stamp = NowIsoUtc();
    std::replace(stamp.begin(), stamp.end(), ':', '-');  // Windows 目录名不吃冒号
    const fs::path backup = memory_dir / ".state" / "migration-backup" / stamp;

    std::vector<StoredEntry> entries = ScanTopics(memory_dir);
    struct Attempt {
        fs::path old_path;
        fs::path new_path;
        std::string original_file;
    };
    std::vector<Attempt> attempts;
    std::size_t migrated = 0;
    const auto rollback = [&attempts, &backup]() {
        // 回退:原地改写的从备份还原(原件已被原子替换盖掉),挪了名的只删
        // 新文件——旧文件从头到尾没动过。
        std::error_code ec;
        for (const Attempt& attempt : attempts) {
            if (attempt.old_path == attempt.new_path) {
                fs::copy_file(backup / Utf8Path(attempt.original_file), attempt.old_path,
                              fs::copy_options::overwrite_existing, ec);
            } else {
                fs::remove(attempt.new_path, ec);
            }
        }
    };
    for (auto& stored : entries) {
        if (stored.public_entry.schema >= 3 || stored.public_entry.status == "conflict") continue;
        const fs::path old_path = memory_dir / Utf8Path(stored.public_entry.file);
        const std::string original_file = stored.public_entry.file;
        const std::string body =
            frontmatter::StripTitleHeading(frontmatter::StripTopicMetadata(ReadFile(old_path)));
        stored.public_entry.schema = 3;
        stored.public_entry.name =
            NameFromId(stored.public_entry.id, MemoryKindName(stored.public_entry.kind));
        if (stored.public_entry.created_at.empty()) {
            stored.public_entry.created_at = stored.public_entry.updated_at;
        }
        stored.public_entry.file =
            CanonicalTopicFile(stored.public_entry.kind, stored.public_entry.name);
        const fs::path new_path = memory_dir / Utf8Path(stored.public_entry.file);

        // 原件先备进 .state/migration-backup/<时间>/,按原相对路径镜像。
        std::error_code ec;
        const fs::path keep = backup / Utf8Path(original_file);
        fs::create_directories(keep.parent_path(), ec);
        if (ec) return std::unexpected("创建迁移备份目录失败: " + ec.message());
        fs::copy_file(old_path, keep, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            rollback();
            return std::unexpected("迁移备份失败: " + PathUtf8(old_path) + ": " + ec.message());
        }

        auto result = AtomicWrite(new_path, BuildTopicText(stored, body));
        if (!result.has_value()) {
            rollback();
            return std::unexpected("迁移写入失败(已回退): " + result.error());
        }
        attempts.push_back(Attempt{old_path, new_path, original_file});
        ++migrated;
    }

    // 全部写妥才动旧文件与派生物;失败时旧文件仍在,catalog 仍可用。
    for (const Attempt& attempt : attempts) {
        if (attempt.old_path == attempt.new_path) continue;
        std::error_code ec;
        fs::remove(attempt.old_path, ec);
        if (ec) {
            return std::unexpected("迁移清理旧文件失败: " + PathUtf8(attempt.old_path) + ": " +
                                   ec.message());
        }
    }
    auto rebuilt = RebuildMemoryIndex(memory_dir);
    if (!rebuilt.has_value()) {
        rollback();
        return std::unexpected(rebuilt.error());
    }
    return ProjectMemory::MigrationResult{migrated, PathUtf8(backup)};
}

}  // namespace store

// 测试与 /memory rebuild 共用的同步底层(公共口,声明在 project_memory.hpp)。
// 不起进程。user_layer=true 时按用户层扫描(preferences/feedback,没有
// facts),index 头写 User Memory。
std::expected<void, std::string> RebuildMemoryIndex(const fs::path& memory_dir, bool user_layer) {
    std::vector<std::string> warnings;
    const char* layer = user_layer ? "user" : "project";
    const auto entries = store::ScanTopics(memory_dir, &warnings, layer);
    nlohmann::json catalog{{"schema", 1}, {"generated_at", NowIsoUtc()}, {"entries", nlohmann::json::array()}};
    for (const auto& entry : entries) {
        nlohmann::json item = EntryMetadata(entry);
        item["file"] = entry.public_entry.file;
        catalog["entries"].push_back(std::move(item));
    }
    if (!warnings.empty()) catalog["warnings"] = warnings;
    auto catalog_write = AtomicWrite(memory_dir / ".state" / "catalog.json", catalog.dump(2) + "\n");
    if (!catalog_write.has_value()) return catalog_write;
    return AtomicWrite(memory_dir / "index.md", BuildIndex(entries, layer));
}

}  // namespace lubancode::memory
