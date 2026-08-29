// 候选态的实现(自进化闭环阶段 2)。序列化全走 nlohmann::json;读盘只走
// 本文件的窄口,坏文件跳过不抛。
#include "evolution/candidate.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "package/inventory.hpp"
#include "platform/paths.hpp"

namespace lubancode::evolution {

namespace {

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
    const auto text = ReadTextFile(path);
    if (!text.has_value()) {
        return {};
    }
    std::vector<std::string> lines;
    std::string line;
    std::istringstream stream(*text);
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> StringArray(const nlohmann::json& parent, const char* key) {
    std::vector<std::string> out;
    const auto it = parent.find(key);
    if (it == parent.end() || !it->is_array()) {
        return out;
    }
    for (const auto& item : *it) {
        if (item.is_string()) {
            out.push_back(item.get<std::string>());
        }
    }
    return out;
}

std::string GetString(const nlohmann::json& parent, const char* key) {
    const auto it = parent.find(key);
    if (it == parent.end() || !it->is_string()) {
        return std::string();
    }
    return it->get<std::string>();
}

}  // namespace

// ---------------------------------------------------------------------------
// 状态机
// ---------------------------------------------------------------------------

std::string ToString(CandidateState state) {
    switch (state) {
        case CandidateState::Observed: return "observed";
        case CandidateState::Drafted: return "drafted";
        case CandidateState::Validated: return "validated";
        case CandidateState::Evaluated: return "evaluated";
        case CandidateState::AwaitingApproval: return "awaiting_approval";
        case CandidateState::Staged: return "staged";
        case CandidateState::Canary: return "canary";
        case CandidateState::Active: return "active";
        case CandidateState::Rejected: return "rejected";
        case CandidateState::RolledBack: return "rolled_back";
    }
    return "drafted";
}

std::optional<CandidateState> ParseCandidateState(const std::string& text) {
    static const std::pair<const char*, CandidateState> kTable[] = {
        {"observed", CandidateState::Observed},       {"drafted", CandidateState::Drafted},
        {"validated", CandidateState::Validated},     {"evaluated", CandidateState::Evaluated},
        {"awaiting_approval", CandidateState::AwaitingApproval},
        {"staged", CandidateState::Staged},           {"canary", CandidateState::Canary},
        {"active", CandidateState::Active},           {"rejected", CandidateState::Rejected},
        {"rolled_back", CandidateState::RolledBack},
    };
    for (const auto& [name, state] : kTable) {
        if (text == name) {
            return state;
        }
    }
    return std::nullopt;
}

bool IsTerminalCandidateState(CandidateState state) {
    return state == CandidateState::Rejected || state == CandidateState::RolledBack;
}

bool IsValidCandidateTransition(CandidateState from, CandidateState to) {
    if (IsTerminalCandidateState(from)) {
        return false;  // 终态不再迁移
    }
    if (to == CandidateState::Rejected) {
        return true;  // 任意非终态 -> rejected
    }
    if (to == CandidateState::RolledBack) {
        return from == CandidateState::Canary || from == CandidateState::Active;
    }
    // 线性主路:只许紧邻往前一步。跳步、回退一律 false。
    switch (from) {
        case CandidateState::Observed: return to == CandidateState::Drafted;
        case CandidateState::Drafted: return to == CandidateState::Validated;
        case CandidateState::Validated: return to == CandidateState::Evaluated;
        case CandidateState::Evaluated: return to == CandidateState::AwaitingApproval;
        case CandidateState::AwaitingApproval: return to == CandidateState::Staged;
        case CandidateState::Staged: return to == CandidateState::Canary;
        case CandidateState::Canary: return to == CandidateState::Active;
        case CandidateState::Active: return false;
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// evolution.json
// ---------------------------------------------------------------------------

std::string SerializeEvolutionRecord(const EvolutionRecord& record) {
    nlohmann::json out;
    out["schema"] = record.schema;
    out["candidate_id"] = record.candidate_id;
    out["package_id"] = record.package_id;
    out["candidate_version"] = record.candidate_version;
    if (record.parent.has_value()) {
        out["parent"] = {{"version", record.parent->version},
                         {"content_hash", record.parent->content_hash}};
    } else {
        out["parent"] = nullptr;  // 无父明写 null,不可假装是升级
    }
    out["objective"] = record.objective;
    out["sources"] = {{"run_ids", record.sources.run_ids},
                      {"goal_ids", record.sources.goal_ids},
                      {"recording_ids", record.sources.recording_ids},
                      {"memory_ids", record.sources.memory_ids},
                      {"user_feedback_ids", record.sources.user_feedback_ids}};
    out["generator"] = {{"provider", record.generator.provider},
                        {"model", record.generator.model},
                        {"prompt_revision", record.generator.prompt_revision}};
    out["changes"] = {{"components_added", record.changes.components_added},
                      {"components_changed", record.changes.components_changed},
                      {"components_removed", record.changes.components_removed},
                      {"permissions_added", record.changes.permissions_added},
                      {"tools_added", record.changes.tools_added}};
    out["created_at"] = record.created_at;
    return out.dump(2) + "\n";
}

std::optional<EvolutionRecord> ParseEvolutionRecord(const std::string& text) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    if (!root.is_object() || !root.contains("schema") || !root["schema"].is_number_integer() ||
        root["schema"].get<int>() != 1) {
        return std::nullopt;
    }
    EvolutionRecord record;
    record.candidate_id = GetString(root, "candidate_id");
    record.package_id = GetString(root, "package_id");
    record.candidate_version = GetString(root, "candidate_version");
    record.objective = GetString(root, "objective");
    record.created_at = GetString(root, "created_at");
    if (record.candidate_id.empty() || record.package_id.empty() || record.candidate_version.empty() ||
        record.objective.empty()) {
        return std::nullopt;
    }
    const auto parent = root.find("parent");
    if (parent != root.end() && parent->is_object()) {
        EvolutionRecordParent parsed;
        parsed.version = GetString(*parent, "version");
        parsed.content_hash = GetString(*parent, "content_hash");
        if (!parsed.version.empty() && !parsed.content_hash.empty()) {
            record.parent = parsed;
        }
    }  // null 或缺键:无父,保持 nullopt
    const auto sources = root.find("sources");
    if (sources != root.end() && sources->is_object()) {
        record.sources.run_ids = StringArray(*sources, "run_ids");
        record.sources.goal_ids = StringArray(*sources, "goal_ids");
        record.sources.recording_ids = StringArray(*sources, "recording_ids");
        record.sources.memory_ids = StringArray(*sources, "memory_ids");
        record.sources.user_feedback_ids = StringArray(*sources, "user_feedback_ids");
    }
    const auto generator = root.find("generator");
    if (generator != root.end() && generator->is_object()) {
        record.generator.provider = GetString(*generator, "provider");
        record.generator.model = GetString(*generator, "model");
        record.generator.prompt_revision = GetString(*generator, "prompt_revision");
    }
    const auto changes = root.find("changes");
    if (changes != root.end() && changes->is_object()) {
        record.changes.components_added = StringArray(*changes, "components_added");
        record.changes.components_changed = StringArray(*changes, "components_changed");
        record.changes.components_removed = StringArray(*changes, "components_removed");
        record.changes.permissions_added = StringArray(*changes, "permissions_added");
        record.changes.tools_added = StringArray(*changes, "tools_added");
    }
    return record;
}

// ---------------------------------------------------------------------------
// approval.json
// ---------------------------------------------------------------------------

std::string SerializeApprovalRecord(const ApprovalRecord& approval) {
    nlohmann::json out;
    out["schema"] = approval.schema;
    out["candidate_id"] = approval.candidate_id;
    out["package_id"] = approval.package_id;
    out["candidate_version"] = approval.candidate_version;
    out["content_hash"] = approval.content_hash;
    out["tier"] = approval.tier;
    out["status"] = approval.status;
    out["requested_at"] = approval.requested_at;
    if (approval.decision.has_value()) {
        out["decision"] = {{"decided_by", approval.decision->decided_by},
                           {"decision", approval.decision->decision},
                           {"decided_at", approval.decision->decided_at},
                           {"reason", approval.decision->reason},
                           {"fingerprint", approval.decision->fingerprint}};
    } else {
        out["decision"] = nullptr;
    }
    return out.dump(2) + "\n";
}

std::optional<ApprovalRecord> ParseApprovalRecord(const std::string& text) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    if (!root.is_object() || !root.contains("schema") || !root["schema"].is_number_integer() ||
        root["schema"].get<int>() != 1) {
        return std::nullopt;
    }
    ApprovalRecord approval;
    approval.candidate_id = GetString(root, "candidate_id");
    approval.package_id = GetString(root, "package_id");
    approval.candidate_version = GetString(root, "candidate_version");
    approval.content_hash = GetString(root, "content_hash");
    approval.tier = GetString(root, "tier");
    approval.status = GetString(root, "status");
    approval.requested_at = GetString(root, "requested_at");
    if (approval.candidate_id.empty() || approval.status.empty()) {
        return std::nullopt;
    }
    const auto decision = root.find("decision");
    if (decision != root.end() && decision->is_object()) {
        ApprovalDecision parsed;
        parsed.decided_by = GetString(*decision, "decided_by");
        parsed.decision = GetString(*decision, "decision");
        parsed.decided_at = GetString(*decision, "decided_at");
        parsed.reason = GetString(*decision, "reason");
        parsed.fingerprint = GetString(*decision, "fingerprint");
        if (!parsed.decided_by.empty() && !parsed.decision.empty()) {
            approval.decision = parsed;
        }
    }
    return approval;
}

// ---------------------------------------------------------------------------
// state.jsonl
// ---------------------------------------------------------------------------

std::string SerializeStateEntry(const CandidateStateEntry& entry) {
    nlohmann::json out;
    out["schema"] = entry.schema;
    out["seq"] = entry.seq;
    out["from"] = ToString(entry.from);
    out["to"] = ToString(entry.to);
    out["actor"] = entry.actor;
    out["reason"] = entry.reason;
    out["at"] = entry.at;
    if (!entry.fingerprint.empty()) {
        out["fingerprint"] = entry.fingerprint;
    }
    return out.dump();
}

std::optional<CandidateStateEntry> ParseStateEntry(const std::string& line) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(line);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    if (!root.is_object() || !root.contains("schema") || !root["schema"].is_number_integer() ||
        root["schema"].get<int>() != 1) {
        return std::nullopt;
    }
    const auto to_state = ParseCandidateState(GetString(root, "to"));
    if (!to_state.has_value()) {
        return std::nullopt;
    }
    CandidateStateEntry entry;
    entry.to = *to_state;
    if (const auto from_state = ParseCandidateState(GetString(root, "from")); from_state.has_value()) {
        entry.from = *from_state;
    }
    if (root.contains("seq") && root["seq"].is_number_integer()) {
        entry.seq = root["seq"].get<std::int64_t>();
    }
    entry.actor = GetString(root, "actor");
    entry.reason = GetString(root, "reason");
    entry.at = GetString(root, "at");
    entry.fingerprint = GetString(root, "fingerprint");
    return entry;
}

// ---------------------------------------------------------------------------
// 候选仓
// ---------------------------------------------------------------------------

CandidateStore::CandidateStore(std::filesystem::path root_dir) : root_(std::move(root_dir)) {}

std::filesystem::path CandidateStore::CandidateDir(const std::string& package_id,
                                                   const std::string& candidate_id) const {
    return root_ / lubancode::platform::Utf8ToPath(package_id) /
           lubancode::platform::Utf8ToPath(candidate_id);
}

CandidateState CandidateStore::ReadState(const std::filesystem::path& candidate_dir) {
    std::optional<CandidateState> last;
    for (const std::string& line : ReadLines(candidate_dir / "state.jsonl")) {
        if (const auto entry = ParseStateEntry(line); entry.has_value()) {
            last = entry->to;
        }
    }
    if (last.has_value()) {
        return *last;
    }
    // 账缺失的回落:手工夹具/旧候选。approval 已 rejected 给 rejected,
    // evolution.json 在(即起草落过笔)给 drafted。
    if (const auto text = ReadTextFile(candidate_dir / "approval.json"); text.has_value()) {
        if (const auto approval = ParseApprovalRecord(*text); approval.has_value() &&
            approval->status == "rejected") {
            return CandidateState::Rejected;
        }
    }
    if (std::filesystem::exists(candidate_dir / "evolution.json")) {
        return CandidateState::Drafted;
    }
    return CandidateState::Observed;
}

std::vector<CandidateSummary> CandidateStore::LoadAll() const {
    std::vector<CandidateSummary> out;
    std::error_code ec;
    if (!std::filesystem::exists(root_, ec)) {
        return out;
    }
    for (const auto& package_entry : std::filesystem::directory_iterator(root_, ec)) {
        if (!package_entry.is_directory()) {
            continue;
        }
        const std::string package_id = lubancode::platform::PathToUtf8(package_entry.path().filename());
        std::error_code inner_ec;
        for (const auto& candidate_entry :
             std::filesystem::directory_iterator(package_entry.path(), inner_ec)) {
            if (!candidate_entry.is_directory()) {
                continue;
            }
            const std::string candidate_id =
                lubancode::platform::PathToUtf8(candidate_entry.path().filename());
            if (candidate_id.rfind("cand-", 0) != 0) {
                continue;
            }
            const std::filesystem::path dir = candidate_entry.path();
            const auto record_text = ReadTextFile(dir / "evolution.json");
            if (!record_text.has_value()) {
                continue;  // 残缺目录(起草到一半崩掉),不算候选
            }
            CandidateSummary summary;
            summary.package_id = package_id;
            summary.candidate_id = candidate_id;
            summary.dir = dir;
            summary.state = ReadState(dir);
            summary.record = ParseEvolutionRecord(*record_text);
            if (const auto text = ReadTextFile(dir / "approval.json"); text.has_value()) {
                summary.approval = ParseApprovalRecord(*text);
            }
            summary.content_hash = ComputeCandidateContentHash(dir / "package");
            if (summary.record.has_value()) {
                // 记账优先用 evolution.json 的身份;盘面目录名只是住址。
                summary.package_id = summary.record->package_id;
                summary.candidate_id = summary.record->candidate_id;
            }
            out.push_back(std::move(summary));
        }
    }
    std::sort(out.begin(), out.end(), [](const CandidateSummary& a, const CandidateSummary& b) {
        if (a.package_id != b.package_id) {
            return a.package_id < b.package_id;
        }
        return a.candidate_id < b.candidate_id;
    });
    return out;
}

std::optional<CandidateSummary> CandidateStore::Find(const std::string& candidate_id) const {
    for (const CandidateSummary& summary : LoadAll()) {
        if (summary.candidate_id == candidate_id) {
            return summary;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 内容哈希
// ---------------------------------------------------------------------------

std::string ComputeCandidateContentHash(const std::filesystem::path& package_dir) {
    std::error_code ec;
    if (!std::filesystem::exists(package_dir / "package.yaml", ec)) {
        return std::string();
    }
    package::PackageCandidate candidate;
    candidate.scope = package::PackageScope::Dev;  // 只影响台账显示,不进哈希
    candidate.layer_root = package_dir.parent_path();
    candidate.package_root = package_dir;
    candidate.dir_name = lubancode::platform::PathToUtf8(package_dir.filename());
    const package::PackageInventory inventory = package::BuildPackageInventory(candidate);
    if (inventory.content_hash.empty()) {
        return std::string();
    }
    return "sha256:" + inventory.content_hash;
}

}  // namespace lubancode::evolution
