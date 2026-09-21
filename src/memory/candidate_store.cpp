// candidate_store.hpp 的实现。SV-09(2026-09-21 架构审查)拆分前住
// project_memory.cpp,搬来逻辑一字未动;原 ProjectMemory::AddCandidate 的
// generate_enabled 授权闸留在门面(授权规则一份),箱只管文件与账本。

#include "memory/candidate_store.hpp"

#include <algorithm>
#include <system_error>

#include <nlohmann/json.hpp>

#include "memory/internal.hpp"
#include "memory/topic_store.hpp"  // store::ValidateSaveRequest:与正式保存同一套校验

namespace lubancode::memory::candidates {

namespace {

namespace fs = std::filesystem;

nlohmann::json CandidateToJson(const MemoryCandidate& candidate) {
    return nlohmann::json{
        {"schema", 1},
        {"id", candidate.id},
        {"kind", MemoryKindName(candidate.kind)},
        {"title", candidate.title},
        {"summary", candidate.summary},
        {"content", candidate.content},
        {"keywords", candidate.keywords},
        {"paths", candidate.paths},
        {"confidence", candidate.confidence},
        {"task_type", candidate.task_type},
        {"created_at", candidate.created_at},
        {"occurred_at", candidate.occurred_at},
    };
}

std::optional<MemoryCandidate> CandidateFromJson(const nlohmann::json& root) {
    if (!root.is_object() || root.value("schema", 0) != 1) return std::nullopt;
    MemoryCandidate candidate;
    candidate.id = root.value("id", std::string());
    auto kind = ParseMemoryKind(root.value("kind", std::string()));
    if (!kind.has_value() || candidate.id.empty()) return std::nullopt;
    candidate.kind = *kind;
    candidate.title = root.value("title", std::string());
    candidate.summary = root.value("summary", std::string());
    candidate.content = root.value("content", std::string());
    candidate.confidence = root.value("confidence", std::string("inferred"));
    candidate.task_type = root.value("task_type", std::string("other"));
    candidate.created_at = root.value("created_at", std::string());
    // 时间线锚点:旧候选无此键读空;形状不像日期按空(不造假)。
    const std::string occurred = root.value("occurred_at", std::string());
    if (LooksLikeMemoryDate(occurred)) candidate.occurred_at = occurred;
    if (root.contains("keywords") && root["keywords"].is_array()) {
        for (const auto& item : root["keywords"]) {
            if (item.is_string()) candidate.keywords.push_back(item.get<std::string>());
        }
    }
    if (root.contains("paths") && root["paths"].is_array()) {
        for (const auto& item : root["paths"]) {
            if (item.is_string()) candidate.paths.push_back(item.get<std::string>());
        }
    }
    if (candidate.title.empty() || candidate.content.empty()) return std::nullopt;
    return candidate;
}

// 查重键:kind + 归一化标题(小写、压空白)。拒绝账本也用同一枚哈希,
// 同主题候选拒绝后不再死缠。
std::string CandidateSubjectKey(MemoryKind kind, const std::string& title) {
    std::string normalized = LowerAscii(Trim(title));
    std::string squeezed;
    bool in_space = false;
    for (const char c : normalized) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            in_space = !squeezed.empty();
            continue;
        }
        if (in_space) {
            squeezed.push_back(' ');
            in_space = false;
        }
        squeezed.push_back(c);
    }
    return MemoryKindName(kind) + "\n" + squeezed;
}

}  // namespace

fs::path CandidatesDir(const fs::path& memory_dir) {
    // P0-3:候选审阅箱搬进 workspace memory 树(合同 §一 layout 的
    // memory/memory-candidates/),不再住旧 <projects>/<key>/。
    return memory_dir / "memory-candidates";
}

std::vector<MemoryCandidate> ListCandidates(const fs::path& memory_dir) {
    std::vector<MemoryCandidate> out;
    std::error_code ec;
    fs::directory_iterator it(CandidatesDir(memory_dir), ec);
    if (ec) return out;
    for (const auto& item : it) {
        if (!item.is_regular_file(ec) || item.path().extension() != ".json") continue;
        try {
            auto candidate = CandidateFromJson(nlohmann::json::parse(ReadFile(item.path())));
            if (candidate.has_value()) out.push_back(std::move(*candidate));
        } catch (const nlohmann::json::exception&) {
            // 坏候选文件跳过,不拦列表。
        }
    }
    std::sort(out.begin(), out.end(), [](const MemoryCandidate& a, const MemoryCandidate& b) {
        if (a.created_at != b.created_at) return a.created_at < b.created_at;
        return a.id < b.id;
    });
    return out;
}

std::optional<MemoryCandidate> GetCandidate(const fs::path& memory_dir, const std::string& id) {
    for (const auto& candidate : ListCandidates(memory_dir)) {
        if (candidate.id == id) return candidate;
    }
    return std::nullopt;
}

std::expected<std::string, std::string> AddCandidate(const fs::path& memory_dir,
                                                     MemoryCandidate candidate) {
    // 候选字段过一遍与正式保存同一套校验(长度、敏感内容、路径)。
    SaveRequest probe;
    probe.kind = candidate.kind;
    probe.title = candidate.title;
    probe.summary = candidate.summary;
    probe.content = candidate.content;
    probe.keywords = candidate.keywords;
    probe.paths = candidate.paths;
    if (auto valid = store::ValidateSaveRequest(probe); !valid.has_value()) {
        return std::unexpected(valid.error());
    }

    const std::string subject = CandidateSubjectKey(candidate.kind, candidate.title);
    const std::string subject_hash = HexHash(subject);

    // 拒绝账本先查:同主题拒过的不再收。
    nlohmann::json rejected;
    try {
        rejected = nlohmann::json::parse(ReadFile(CandidatesDir(memory_dir) / "rejected.json"));
    } catch (const nlohmann::json::exception&) {
        rejected = nlohmann::json::object();
    }
    if (rejected.is_object() && rejected.contains(subject_hash)) {
        return std::unexpected("同主题候选此前已拒绝: " + candidate.title);
    }

    // 待审区查重:同主题原位更新(沿用 id 与创建时间)。
    for (auto& existing : ListCandidates(memory_dir)) {
        if (CandidateSubjectKey(existing.kind, existing.title) == subject) {
            candidate.id = existing.id;
            candidate.created_at = existing.created_at;
            auto written = AtomicWrite(CandidatesDir(memory_dir) / Utf8Path(candidate.id + ".json"),
                                       CandidateToJson(candidate).dump(2) + "\n");
            if (!written.has_value()) return std::unexpected(written.error());
            return candidate.id;
        }
    }

    candidate.id = "cand-" + JobStamp();
    candidate.created_at = NowIsoUtc();
    auto written =
        AtomicWrite(CandidatesDir(memory_dir) / Utf8Path(candidate.id + ".json"), CandidateToJson(candidate).dump(2) + "\n");
    if (!written.has_value()) return std::unexpected(written.error());
    return candidate.id;
}

std::expected<void, std::string> EditCandidate(const fs::path& memory_dir, const std::string& id,
                                               const std::string& title, const std::string& content) {
    auto candidate = GetCandidate(memory_dir, id);
    if (!candidate.has_value()) return std::unexpected("找不到候选: " + id);
    if (!title.empty()) candidate->title = OneLine(title, kMaxTitleBytes);
    if (!content.empty()) candidate->content = content;
    candidate->summary = OneLine(candidate->summary.empty() ? candidate->content : candidate->summary,
                                 kMaxSummaryBytes);
    SaveRequest probe;
    probe.kind = candidate->kind;
    probe.title = candidate->title;
    probe.summary = candidate->summary;
    probe.content = candidate->content;
    probe.keywords = candidate->keywords;
    probe.paths = candidate->paths;
    if (auto valid = store::ValidateSaveRequest(probe); !valid.has_value()) return valid;
    return AtomicWrite(CandidatesDir(memory_dir) / Utf8Path(id + ".json"), CandidateToJson(*candidate).dump(2) + "\n");
}

std::expected<void, std::string> RejectCandidate(const fs::path& memory_dir, const std::string& id,
                                                 std::string reason) {
    auto candidate = GetCandidate(memory_dir, id);
    if (!candidate.has_value()) return std::unexpected("找不到候选: " + id);
    if (reason.empty()) reason = "user-rejected";

    std::error_code ec;
    fs::remove(CandidatesDir(memory_dir) / Utf8Path(id + ".json"), ec);

    // 只留短哈希与理由;若同主题还有别的候选,一并清掉同主题的,防死缠。
    const std::string subject = CandidateSubjectKey(candidate->kind, candidate->title);
    const std::string subject_hash = HexHash(subject);
    for (auto& other : ListCandidates(memory_dir)) {
        if (CandidateSubjectKey(other.kind, other.title) == subject) {
            fs::remove(CandidatesDir(memory_dir) / Utf8Path(other.id + ".json"), ec);
        }
    }
    nlohmann::json rejected;
    try {
        rejected = nlohmann::json::parse(ReadFile(CandidatesDir(memory_dir) / "rejected.json"));
    } catch (const nlohmann::json::exception&) {
        rejected = nlohmann::json::object();
    }
    if (!rejected.is_object()) rejected = nlohmann::json::object();
    rejected[subject_hash] = nlohmann::json{
        {"reason", OneLine(reason, 200)},
        {"title", OneLine(candidate->title, kMaxTitleBytes)},
        {"at", NowIsoUtc()},
    };
    return AtomicWrite(CandidatesDir(memory_dir) / "rejected.json", rejected.dump(2) + "\n");
}

void RemoveCandidateFile(const fs::path& memory_dir, const std::string& id) {
    std::error_code ec;
    fs::remove(CandidatesDir(memory_dir) / Utf8Path(id + ".json"), ec);
}

}  // namespace lubancode::memory::candidates
