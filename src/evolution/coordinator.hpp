// 自进化闭环阶段 2:EvolutionCoordinator——候选状态机的唯一写口。
//
// 契约铁律(README"候选状态机"):状态只许这一枚改。CLI、TUI、Workflow、
// Agent 都不得各写一套迁移规矩。本类的职责:
//   - ProposeRecording:一场录制 -> 最小 content-only 候选落盘(候选只落
//     candidate store,不进 PackageCatalog,不进四层扫描目录);
//   - Reject:任意非终态 -> rejected,approval.json 记 decision(含去重
//     fingerprint),观察账 MarkRejected(被拒 fingerprint 不再重复进账,
//     也不可再起草同类);
//   - Diff:与父版或空对照,列新增文件与 SKILL 正文摘要(只读)。
//
// 写盘次序有讲究:先 package/ 两份文本,复算整包哈希,再落 evolution.json、
// approval.json、eval-plan.json、eval-results.jsonl,最后写 state.jsonl 首行
// (observed->drafted)。写到一半崩掉、连演化账都没落上的残缺目录,
// CandidateStore::LoadAll 不认它作候选;落了演化账却缺状态账的,读取时按
// drafted 回落,缺哪份账 show 页就少哪节,不假装齐。
#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "evolution/candidate.hpp"
#include "evolution/eval.hpp"
#include "evolution/observation_store.hpp"
#include "skills/workflow_recorder.hpp"

namespace lubancode::evolution {

class EvolutionCoordinator {
public:
    // candidates_root 即 package-candidates/;observations 可空(纯离线测试),
    // 空时 propose 不落观察账、reject 不记去重指纹(只改候选自己的账)。
    EvolutionCoordinator(std::filesystem::path candidates_root, ObservationStore* observations);

    struct ProposeResult {
        std::string package_id;
        std::string candidate_id;
        std::string candidate_version;
        std::string content_hash;
        std::filesystem::path candidate_dir;
        std::string skill_rel_path;  // 包内相对路径 skills/<id>/SKILL.md
    };

    // 起草并落候选。观察先过拒绝门(被拒 fingerprint 的同类不再起草);
    // 观察不在账里就顺手落账(重采幂等,由 ObservationStore 把守)。
    std::expected<ProposeResult, std::string> ProposeRecording(
        const skills::RecordingStatus& status, const std::vector<skills::RecordEvent>& events);

    struct RejectResult {
        std::string fingerprint;
        std::filesystem::path candidate_dir;
    };

    // 拒绝一只候选。终态候选不可再迁移;fingerprint 优先取来源观察的同类
    // 指纹(观察账在才有),取不到回落到包 id + 当前内容哈希。
    std::expected<RejectResult, std::string> Reject(const std::string& candidate_id,
                                                    const std::string& reason);

    // ---- 评测(阶段 3):五道门跑完,结果只追加进 eval-results.jsonl ----

    struct TestOptions {
        // CI 的 --baseline <package-dir>:父版包目录,补一份静态对照
        // (doctor + 哈希对账)。不给就按计划的 baseline 节走。
        std::optional<std::filesystem::path> baseline_package_dir;
    };

    struct TestReport {
        std::string candidate_id;
        std::string package_id;
        std::string content_hash;
        std::filesystem::path candidate_dir;
        std::string state_before;
        std::string state_after;
        bool transitioned_validated = false;  // drafted -> validated(静态门全绿)
        bool transitioned_evaluated = false;  // validated -> evaluated(五道门入账)
        bool plan_loaded = false;
        std::string plan_error;               // 计划读不出/解析失败的人话
        StaticGateResult static_gate;
        std::vector<EvalResultLine> appended;  // 本次追加的行(含 static)
        bool fixture_missing_any = false;      // workspace/基线夹具缺失(CI 退 2)
        EvalSummary run_summary;               // 本次行的汇总
        EvalSummary ledger_summary;            // 追加后整账的汇总(show 用)
        int exit_code = 0;                     // CI 口径:全过 0/有 fail 1/夹具缺失 2
    };

    // 按候选 id 评测(/evolve test)。
    std::expected<TestReport, std::string> Test(const std::string& candidate_id,
                                                const TestOptions& options = TestOptions{});
    // 按候选目录评测(CI:luban evolve test <candidate-dir>)。目录形状须是
    // <root>/<package-id>/<candidate-id>,evolution.json 齐才算候选。
    std::expected<TestReport, std::string> TestDir(const std::filesystem::path& candidate_dir,
                                                   const TestOptions& options = TestOptions{});

    struct DiffFile {
        std::string rel;      // 包内相对路径('/' 分隔)
        std::size_t size = 0;
        std::string hash;     // "sha256:" + 64hex(单文件)
        bool is_skill = false;
    };

    struct DiffResult {
        std::string candidate_id;
        std::string package_id;
        std::string baseline;  // "父版 <pkg>@<ver>(hash …)" 或 "(无父版,与空对照)"
        std::vector<DiffFile> added;
        std::string skill_summary;  // SKILL 正文摘要(节标题 + 要点行)
    };

    // 与父版或空对照(只读)。阶段 2 候选一律无父,即与空对照,全量列新增。
    std::expected<DiffResult, std::string> Diff(const std::string& candidate_id);

    const CandidateStore& store() const { return store_; }

private:
    std::filesystem::path root_;
    ObservationStore* observations_;
    CandidateStore store_;
};

}  // namespace lubancode::evolution
