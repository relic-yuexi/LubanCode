// 自进化闭环阶段 2:候选态——evolution.json / approval.json / eval-plan 的
// schema 1 序列化、候选状态机(纯迁移表)与候选仓(只读盘点)。
//
// 契约(docs/features/evolution/README.md)钉死的边界:
//   - 候选落 ~/.lubancode/package-candidates/<package-id>/<candidate-id>/,
//     与正式 Package 目录(package-store/、packages/)分开。这里只有读侧;
//     写侧唯一口是 EvolutionCoordinator(coordinator.hpp),别处不许落笔。
//   - 状态只许 EvolutionCoordinator 改,每笔迁移落只追加事件账 state.jsonl
//     (README 状态机节"每笔迁移落只追加事件账:谁改、何时、因何");
//     当前状态 = 账里最后一行。坏行/半截行跳过,不废整账。
//   - 内容哈希照 Package 阶段 1 的整包盘点算法复算(inventory.content_hash),
//     这里不另立口径。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::evolution {

// ---------------------------------------------------------------------------
// 状态机(纯)
// ---------------------------------------------------------------------------

enum class CandidateState {
    Observed,
    Drafted,
    Validated,
    Evaluated,
    AwaitingApproval,
    Staged,
    Canary,
    Active,
    Rejected,
    RolledBack,
};
std::string ToString(CandidateState state);
std::optional<CandidateState> ParseCandidateState(const std::string& text);

// 终态两枚:rejected、rolled_back。到终态不再迁移。
bool IsTerminalCandidateState(CandidateState state);

// 合法迁移逐条列死(README 迁移表):
//   线性:observed->drafted->validated->evaluated->awaiting_approval
//        ->staged->canary->active
//   任意非终态 -> rejected;canary/active -> rolled_back。
// 跳步、回退、从终态出发、observed 直上 staged/canary/active 一律非法。
bool IsValidCandidateTransition(CandidateState from, CandidateState to);

// ---------------------------------------------------------------------------
// evolution.json(schema 1)
// ---------------------------------------------------------------------------

struct EvolutionRecordParent {
    std::string version;
    std::string content_hash;  // "sha256:" + 64 hex
};

struct EvolutionRecordSources {
    std::vector<std::string> run_ids;
    std::vector<std::string> goal_ids;
    std::vector<std::string> recording_ids;
    std::vector<std::string> memory_ids;
    std::vector<std::string> user_feedback_ids;
};

struct EvolutionRecordGenerator {
    std::string provider;
    std::string model;
    std::string prompt_revision;
};

struct EvolutionRecordChanges {
    std::vector<std::string> components_added;
    std::vector<std::string> components_changed;
    std::vector<std::string> components_removed;
    std::vector<std::string> permissions_added;
    std::vector<std::string> tools_added;
};

struct EvolutionRecord {
    int schema = 1;
    std::string candidate_id;
    std::string package_id;
    std::string candidate_version;                  // SemVer 预发布段,如 0.1.0-candidate.1
    std::optional<EvolutionRecordParent> parent;    // 无父 = nullopt,序列化成 null
    std::string objective;
    EvolutionRecordSources sources;
    EvolutionRecordGenerator generator;
    EvolutionRecordChanges changes;
    std::string created_at;  // ISO 8601
};

// 整文件 JSON(带缩进,人要看)。schema 不是 1、缺必填(candidate_id/
// package_id/candidate_version/objective)给 nullopt。
std::string SerializeEvolutionRecord(const EvolutionRecord& record);
std::optional<EvolutionRecord> ParseEvolutionRecord(const std::string& text);

// ---------------------------------------------------------------------------
// approval.json(schema 1)
// ---------------------------------------------------------------------------

struct ApprovalDecision {
    std::string decided_by;  // 首版只认 "user"
    std::string decision;    // "approved" / "rejected"
    std::string decided_at;  // ISO 8601
    std::string reason;
    std::string fingerprint;  // 拒绝去重的指纹
};

struct ApprovalRecord {
    int schema = 1;
    std::string candidate_id;
    std::string package_id;
    std::string candidate_version;
    std::string content_hash;  // 批准只认当前 content hash
    std::string tier;          // content-only / process-plugin-or-mcp / native-or-core-patch
    std::string status;        // awaiting_approval / approved / rejected
    std::string requested_at;  // ISO 8601
    std::optional<ApprovalDecision> decision;  // 未决 = nullopt,序列化成 null
};

std::string SerializeApprovalRecord(const ApprovalRecord& approval);
std::optional<ApprovalRecord> ParseApprovalRecord(const std::string& text);

// ---------------------------------------------------------------------------
// state.jsonl(行 schema 1,只追加)
// ---------------------------------------------------------------------------

struct CandidateStateEntry {
    int schema = 1;
    std::int64_t seq = 0;  // 候选内从 1 起递增
    CandidateState from = CandidateState::Observed;
    CandidateState to = CandidateState::Drafted;
    std::string actor;        // 谁改;首版只认 "user"
    std::string reason;       // 因何
    std::string at;           // 何时,ISO 8601
    std::string fingerprint;  // reject 行记去重指纹;其余行空
};

std::string SerializeStateEntry(const CandidateStateEntry& entry);
std::optional<CandidateStateEntry> ParseStateEntry(const std::string& line);

// ---------------------------------------------------------------------------
// 候选仓(只读)
// ---------------------------------------------------------------------------

// 一只候选在盘上的快照。
struct CandidateSummary {
    std::string package_id;
    std::string candidate_id;
    std::filesystem::path dir;                     // <root>/<package-id>/<candidate-id>
    CandidateState state = CandidateState::Drafted;
    std::optional<EvolutionRecord> record;
    std::optional<ApprovalRecord> approval;
    std::string content_hash;  // 复算自 package/;"sha256:"+64hex;package 目录缺了给空
};

class CandidateStore {
public:
    // root 即 package-candidates/ 目录。建不出不报错,写侧(Coordinator)再算账。
    explicit CandidateStore(std::filesystem::path root_dir);

    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path CandidateDir(const std::string& package_id,
                                       const std::string& candidate_id) const;

    // 两层盘点:<root>/<package-id>/<candidate-id>/,须有 evolution.json(起草
    // 到一半崩掉的残缺目录不算候选)。目录不存在给空表。
    std::vector<CandidateSummary> LoadAll() const;
    std::optional<CandidateSummary> Find(const std::string& candidate_id) const;

    // 候选目录当前状态:state.jsonl 最后一行;账缺失时回落——approval 已
    // rejected 给 rejected,evolution.json 在给 drafted(propose 必写首行,
    // 缺账只会出现在手工夹具/旧候选上)。
    static CandidateState ReadState(const std::filesystem::path& candidate_dir);

private:
    std::filesystem::path root_;
};

// ---------------------------------------------------------------------------
// 内容哈希:照 Package 阶段 1 的整包盘点算法复算
// ---------------------------------------------------------------------------

// 对候选的 package/ 目录复算整包哈希(package::BuildPackageInventory 的
// content_hash,同一份算法),返回 "sha256:" + 64 hex。目录读不动给空串。
std::string ComputeCandidateContentHash(const std::filesystem::path& package_dir);

}  // namespace lubancode::evolution
