// 自进化闭环阶段 2:候选态单测。钉五件事——
//   1. 状态机迁移表:合法与非法逐条(跳步、回退、终态出发全拦);
//   2. evolution.json / approval.json schema 1 的序列化回路与阶段 0 冻结
//      夹具形状(tests/fixtures/evolution 两只候选能被解析,parent null 认得);
//   3. state.jsonl 行的序列化回路,坏行跳过;
//   4. 候选仓盘点:两层目录、残缺目录不认、状态回落;
//   5. 内容哈希:与 package 阶段 1 盘点算法同值,内容一字变动哈希即变。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "evolution/candidate.hpp"

#ifndef LUBANCODE_TEST_FIXTURES_DIR
#define LUBANCODE_TEST_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

namespace fs = std::filesystem;
using namespace lubancode::evolution;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_evolution_candidate_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    const fs::path& Get() const { return dir_; }

private:
    fs::path dir_;
};

void WriteFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << content;
}

}  // namespace

// ---------------------------------------------------------------------------
// 状态机
// ---------------------------------------------------------------------------

TEST_CASE("状态机:合法迁移逐条") {
    CHECK(IsValidCandidateTransition(CandidateState::Observed, CandidateState::Drafted));
    CHECK(IsValidCandidateTransition(CandidateState::Drafted, CandidateState::Validated));
    CHECK(IsValidCandidateTransition(CandidateState::Validated, CandidateState::Evaluated));
    CHECK(IsValidCandidateTransition(CandidateState::Evaluated, CandidateState::AwaitingApproval));
    CHECK(IsValidCandidateTransition(CandidateState::AwaitingApproval, CandidateState::Staged));
    CHECK(IsValidCandidateTransition(CandidateState::Staged, CandidateState::Canary));
    CHECK(IsValidCandidateTransition(CandidateState::Canary, CandidateState::Active));
    // 任意非终态 -> rejected。
    for (const CandidateState from : {CandidateState::Observed, CandidateState::Drafted,
                                      CandidateState::Validated, CandidateState::Evaluated,
                                      CandidateState::AwaitingApproval, CandidateState::Staged,
                                      CandidateState::Canary, CandidateState::Active}) {
        INFO(ToString(from));
        CHECK(IsValidCandidateTransition(from, CandidateState::Rejected));
    }
    // canary/active -> rolled_back(已装版本的退出主路)。
    CHECK(IsValidCandidateTransition(CandidateState::Canary, CandidateState::RolledBack));
    CHECK(IsValidCandidateTransition(CandidateState::Active, CandidateState::RolledBack));
}

TEST_CASE("状态机:非法迁移全拦(跳步/回退/终态/observed 直上)") {
    // 跳步。
    CHECK_FALSE(IsValidCandidateTransition(CandidateState::Observed, CandidateState::Validated));
    CHECK_FALSE(IsValidCandidateTransition(CandidateState::Drafted, CandidateState::AwaitingApproval));
    CHECK_FALSE(IsValidCandidateTransition(CandidateState::Evaluated, CandidateState::Staged));
    CHECK_FALSE(IsValidCandidateTransition(CandidateState::Staged, CandidateState::Active));
    // 回退。
    CHECK_FALSE(IsValidCandidateTransition(CandidateState::Validated, CandidateState::Drafted));
    CHECK_FALSE(IsValidCandidateTransition(CandidateState::Canary, CandidateState::AwaitingApproval));
    CHECK_FALSE(IsValidCandidateTransition(CandidateState::Active, CandidateState::Canary));
    // observed 直上后段。
    CHECK_FALSE(IsValidCandidateTransition(CandidateState::Observed, CandidateState::Staged));
    CHECK_FALSE(IsValidCandidateTransition(CandidateState::Observed, CandidateState::Canary));
    CHECK_FALSE(IsValidCandidateTransition(CandidateState::Observed, CandidateState::Active));
    // observed 不许直接 rolled_back(没装过,谈不上回滚;退出走 rejected)。
    CHECK_FALSE(IsValidCandidateTransition(CandidateState::Observed, CandidateState::RolledBack));
    CHECK_FALSE(IsValidCandidateTransition(CandidateState::Drafted, CandidateState::RolledBack));
    // 终态出发的一切迁移。
    for (const CandidateState to : {CandidateState::Drafted, CandidateState::Rejected,
                                    CandidateState::Active, CandidateState::RolledBack}) {
        CHECK_FALSE(IsValidCandidateTransition(CandidateState::Rejected, to));
        CHECK_FALSE(IsValidCandidateTransition(CandidateState::RolledBack, to));
    }
    CHECK(IsTerminalCandidateState(CandidateState::Rejected));
    CHECK(IsTerminalCandidateState(CandidateState::RolledBack));
    CHECK_FALSE(IsTerminalCandidateState(CandidateState::Drafted));
}

// ---------------------------------------------------------------------------
// 序列化回路与冻结夹具
// ---------------------------------------------------------------------------

TEST_CASE("evolution.json:parent null 与来回回路") {
    EvolutionRecord record;
    record.candidate_id = "cand-20260828-001";
    record.package_id = "evolve.demo";
    record.candidate_version = "0.1.0-candidate.1";
    record.parent = std::nullopt;
    record.objective = "把一场录制沉淀成可复用技能";
    record.sources.recording_ids = {"rec-1"};
    record.generator = {"builtin", "skill-drafter", "evolution-stage2"};
    record.changes.components_added = {"skills/demo/SKILL.md"};
    record.created_at = "2026-08-28T09:30:00Z";

    const std::string text = SerializeEvolutionRecord(record);
    // 无父明写 null,不可假装是升级。
    CHECK(text.find("\"parent\": null") != std::string::npos);
    const auto back = ParseEvolutionRecord(text);
    REQUIRE(back.has_value());
    CHECK(back->candidate_id == record.candidate_id);
    CHECK(back->package_id == record.package_id);
    CHECK(back->candidate_version == record.candidate_version);
    CHECK_FALSE(back->parent.has_value());
    CHECK(back->sources.recording_ids == std::vector<std::string>{"rec-1"});
    CHECK(back->generator.provider == "builtin");
    CHECK(back->changes.components_added == std::vector<std::string>{"skills/demo/SKILL.md"});

    // 有父回路。
    record.parent = EvolutionRecordParent{"0.1.0", "sha256:" + std::string(64, '0')};
    const auto with_parent = ParseEvolutionRecord(SerializeEvolutionRecord(record));
    REQUIRE(with_parent.has_value());
    REQUIRE(with_parent->parent.has_value());
    CHECK(with_parent->parent->version == "0.1.0");

    // schema 不是 1 / 缺必填:不认。
    CHECK_FALSE(ParseEvolutionRecord("{\"schema\":2}"));
    CHECK_FALSE(ParseEvolutionRecord("not json"));
    CHECK_FALSE(ParseEvolutionRecord("{\"schema\":1,\"candidate_id\":\"c\"}"));
}

TEST_CASE("夹具:阶段 0 冻结的两只候选形状解析得动") {
    const std::string fixtures = LUBANCODE_TEST_FIXTURES_DIR;
    const fs::path content_only =
        fs::path(fixtures) / "evolution" / "candidate-content-only" / "cand-20260828-001";
    const fs::path code_rejected =
        fs::path(fixtures) / "evolution" / "candidate-code-rejected" / "cand-20260828-002";

    {
        std::ifstream file(content_only / "evolution.json", std::ios::binary);
        std::stringstream buffer;
        buffer << file.rdbuf();
        const auto record = ParseEvolutionRecord(buffer.str());
        REQUIRE(record.has_value());
        CHECK(record->package_id == "moontide.provider-auditor");
        CHECK(record->parent.has_value());
        CHECK(record->sources.recording_ids == std::vector<std::string>{"rec-placeholder-201"});
    }
    {
        std::ifstream file(content_only / "approval.json", std::ios::binary);
        std::stringstream buffer;
        buffer << file.rdbuf();
        const auto approval = ParseApprovalRecord(buffer.str());
        REQUIRE(approval.has_value());
        CHECK(approval->status == "awaiting_approval");
        CHECK(approval->tier == "content-only");
        CHECK_FALSE(approval->decision.has_value());  // 未决:decision null
    }
    {
        std::ifstream file(code_rejected / "approval.json", std::ios::binary);
        std::stringstream buffer;
        buffer << file.rdbuf();
        const auto approval = ParseApprovalRecord(buffer.str());
        REQUIRE(approval.has_value());
        CHECK(approval->status == "rejected");
        REQUIRE(approval->decision.has_value());
        CHECK(approval->decision->decided_by == "user");
        CHECK_FALSE(approval->decision->fingerprint.empty());  // 拒绝账留指纹
    }
}

TEST_CASE("approval.json 与 state.jsonl 回路;坏行跳过") {
    ApprovalRecord approval;
    approval.candidate_id = "cand-x";
    approval.package_id = "evolve.x";
    approval.candidate_version = "0.1.0-candidate.1";
    approval.content_hash = "sha256:" + std::string(64, '0');
    approval.tier = "content-only";
    approval.status = "awaiting_approval";
    approval.requested_at = "2026-08-28T10:20:00Z";
    const std::string text = SerializeApprovalRecord(approval);
    CHECK(text.find("\"decision\": null") != std::string::npos);
    const auto back = ParseApprovalRecord(text);
    REQUIRE(back.has_value());
    CHECK(back->status == "awaiting_approval");
    CHECK_FALSE(back->decision.has_value());

    CandidateStateEntry entry;
    entry.seq = 2;
    entry.from = CandidateState::Drafted;
    entry.to = CandidateState::Rejected;
    entry.actor = "user";
    entry.reason = "不要这只";
    entry.at = "2026-08-28T11:00:00Z";
    entry.fingerprint = "fp-abc";
    const auto entry_back = ParseStateEntry(SerializeStateEntry(entry));
    REQUIRE(entry_back.has_value());
    CHECK(entry_back->to == CandidateState::Rejected);
    CHECK(entry_back->from == CandidateState::Drafted);
    CHECK(entry_back->fingerprint == "fp-abc");
    // 坏行:半截 JSON 与 schema 2 都跳。
    CHECK_FALSE(ParseStateEntry("{\"schema\":1,\"to\":\"rejec"));
    CHECK_FALSE(ParseStateEntry("{\"schema\":2,\"to\":\"rejected\"}"));
    CHECK_FALSE(ParseStateEntry("{\"schema\":1,\"to\":\"nonsense\"}"));
}

// ---------------------------------------------------------------------------
// 候选仓盘点与内容哈希
// ---------------------------------------------------------------------------

TEST_CASE("候选仓:两层盘点、残缺目录不认、状态回落") {
    TempDir temp;
    const fs::path root = temp.Get() / "package-candidates";
    // 一只完整候选(最小可用:evolution.json + package/)。
    WriteFile(root / "evolve.demo" / "cand-20260828-001" / "evolution.json",
              SerializeEvolutionRecord([] {
                  EvolutionRecord record;
                  record.candidate_id = "cand-20260828-001";
                  record.package_id = "evolve.demo";
                  record.candidate_version = "0.1.0-candidate.1";
                  record.objective = "demo";
                  record.created_at = "2026-08-28T00:00:00Z";
                  return record;
              }()));
    WriteFile(root / "evolve.demo" / "cand-20260828-001" / "package" / "package.yaml",
              "schema: 1\nid: evolve.demo\nversion: 0.1.0\nname: Demo\ndescription: demo.\n");
    // 残缺目录:没有 evolution.json(起草到一半的尸体)。
    WriteFile(root / "evolve.demo" / "cand-20260828-002" / "package" / "package.yaml", "junk");
    // 非 cand- 前缀目录:不认。
    WriteFile(root / "evolve.demo" / "scratch" / "evolution.json", "{}");
    // rejected 回落:approval 写 rejected、无状态账。
    WriteFile(root / "evolve.other" / "cand-20260828-003" / "evolution.json",
              SerializeEvolutionRecord([] {
                  EvolutionRecord record;
                  record.candidate_id = "cand-20260828-003";
                  record.package_id = "evolve.other";
                  record.candidate_version = "0.1.0-candidate.1";
                  record.objective = "other";
                  return record;
              }()));
    WriteFile(root / "evolve.other" / "cand-20260828-003" / "approval.json",
              SerializeApprovalRecord([] {
                  ApprovalRecord approval;
                  approval.candidate_id = "cand-20260828-003";
                  approval.package_id = "evolve.other";
                  approval.status = "rejected";
                  return approval;
              }()));

    const CandidateStore store(root);
    const auto all = store.LoadAll();
    REQUIRE(all.size() == 2);
    CHECK(all[0].candidate_id == "cand-20260828-001");  // 排序稳定
    CHECK(all[0].state == CandidateState::Drafted);      // 状态账缺失回落 drafted
    CHECK(all[1].candidate_id == "cand-20260828-003");
    CHECK(all[1].state == CandidateState::Rejected);  // approval rejected 回落
    CHECK(store.Find("cand-20260828-001").has_value());
    CHECK_FALSE(store.Find("cand-20260828-002").has_value());
    CHECK_FALSE(store.Find("nope").has_value());

    // 状态账:最后一行说了算。
    {
        std::ofstream file(root / "evolve.demo" / "cand-20260828-001" / "state.jsonl",
                           std::ios::binary | std::ios::app);
        file << SerializeStateEntry([] {
                 CandidateStateEntry entry;
                 entry.seq = 1;
                 entry.to = CandidateState::Drafted;
                 entry.actor = "user";
                 return entry;
             }()) << "\n"
             << "{\"schema\":1,\"to\":\"rejec\","  // 半截行(无换行残留):跳过
             << "\n"
             << SerializeStateEntry([] {
                    CandidateStateEntry entry;
                    entry.seq = 2;
                    entry.from = CandidateState::Drafted;
                    entry.to = CandidateState::Validated;
                    entry.actor = "user";
                    return entry;
                }()) << "\n";
    }
    CHECK(CandidateStore::ReadState(root / "evolve.demo" / "cand-20260828-001") ==
          CandidateState::Validated);
}

TEST_CASE("内容哈希:照 package 阶段 1 盘点算法;一字变动即变") {
    TempDir temp;
    const fs::path package_dir = temp.Get() / "pkg";
    WriteFile(package_dir / "package.yaml",
              "schema: 1\nid: evolve.demo\nversion: 0.1.0\nname: Demo\ndescription: demo.\n");
    WriteFile(package_dir / "skills" / "demo" / "SKILL.md", "# demo\n正文一\n");
    const std::string hash = ComputeCandidateContentHash(package_dir);
    REQUIRE(hash.rfind("sha256:", 0) == 0);
    REQUIRE(hash.size() == 7 + 64);

    // 同一份目录再算:不漂。
    CHECK(ComputeCandidateContentHash(package_dir) == hash);

    // 改一字:变。
    WriteFile(package_dir / "skills" / "demo" / "SKILL.md", "# demo\n正文二\n");
    CHECK(ComputeCandidateContentHash(package_dir) != hash);

    // 缺 package.yaml:空串。
    CHECK(ComputeCandidateContentHash(temp.Get() / "empty").empty());
}
