// 存储 v2 P0-5:一次性迁移器的验收册。单子 P0-5 验收原文:
//   "故障注入打断迁移一百次,旧源无损;续跑不重复;成功目标全部可
//    Replay/rebuild。"
// 夹具直接吃 tests/fixtures/workspace/legacy/*.jsonl 九件套 + memory/
// (P0-0 冻结的脱敏样本),布置成旧生产布局(~/.lubancode/sessions、
// projects/<old_key>/memory)后走 plan/run/status 三口。
//
// 分册:
//   1. plan:列源、写 intent、只读不动源;
//   2. run:全件 imported,目标 verify+replay 过,manifest 合同键齐;
//   3. 幂等:同源重跑 already_imported,不再造第二份;
//   4. committed operation 拒重跑(migration.result_exists);
//   5. plan 后源被改:该件 source_sha_mismatch,整批不中断;
//   6. delete-source:无 --yes 拒;有 --yes 只删已核验源;
//   7. memory unmappable 列账;
//   8. status:pending/committed/unmappable 账;
//   9. fault hook 打断一百次(核心验收线);
//   10. 真进程 kill 折算(强杀 legacy-storage-migrator,如实记录)。
//
// 迁移器合法持有旧路径字样与旧格式解析(P0-6 删码不陪葬),本册测的是
// 它的行为合同,不是旧格式本身——旧档形状的守门在 test_workspace_fixtures。
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/hash.hpp"
#include "memory/frontmatter.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "trajectory/directory.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/safety.hpp"
#include "workspace/identity.hpp"
#include "workspace/manifest.hpp"
#include "workspace/storage_migrator.hpp"

using namespace lubancode;
namespace fs = std::filesystem;
namespace migrator = workspace::migrator;

namespace {

fs::path FixtureRoot() {
    return fs::path(LUBANCODE_TEST_FIXTURES_DIR) / "workspace";
}

fs::path TempRoot(const std::string& name) {
    const fs::path root = fs::temp_directory_path() / ("lubancode-migrator-" + name);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

void CopyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    for (const auto& entry : fs::recursive_directory_iterator(from, ec)) {
        if (ec) break;
        std::error_code file_ec;
        if (entry.is_regular_file(file_ec) && !file_ec) {
            const fs::path target = to / entry.path().lexically_relative(from);
            fs::create_directories(target.parent_path(), ec);
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec);
        }
    }
}

std::string ReadAll(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::string();
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteFile(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

// 旧源侧的逐字节指纹:相对路径(UTF-8) -> 全文 sha256。断言"旧源一字
// 不动"就用它前后对账(顺带把多出来的文件也算进去——多文件同样是动)。
std::map<std::string, std::string> HashTree(const fs::path& root) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) {
        return out;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (ec) break;
        std::error_code file_ec;
        if (entry.is_regular_file(file_ec) && !file_ec) {
            out[platform::PathToUtf8(entry.path().lexically_relative(root))] =
                hooks::Sha256Hex(ReadAll(entry.path()));
        }
    }
    return out;
}

// 夹具 meta.cwd 的占位路径(不存在于本机,走 cwd_fallback 裁决)。
constexpr const char* kDemoCwd = "C:/Users/sandbox/work/demo-repo";

// 夹具场景名 -> 旧生产 session id(YYYYMMDD-HHMMSS-XXXXXX,时间取各夹具
// meta.started_at)。真实旧档文件名就是这个形状;目标场旧 id 原样带入
//(§2D),断言也用它对账。
std::string LegacySessionId(const std::string& scenario) {
    static const std::map<std::string, std::string> kIds = {
        {"plain-conversation", "20260115-093000-a37f2c"},
        {"tool-roundtrip", "20260116-100000-b91d04"},
        {"mcp-rich-result", "20260117-140000-c58e19"},
        {"subagent-foreground", "20260118-090000-d42b7a"},
        {"subagent-background", "20260119-110000-e6c93f"},
        {"compact", "20260120-150000-f17d55"},
        {"resume", "20260121-090000-0ab8e2"},
        {"linked-worktree", "20260123-130000-1c9f37"},
    };
    const auto it = kIds.find(scenario);
    return it != kIds.end() ? it->second : scenario;
}

// 布置一只旧生产 home:
//   sessions/<name>.jsonl            九件套里点名的那几场
//   sessions/mcp-rich-result/mcp-artifacts/   rich 块 artifact 字节
//   sessions/archive/archived-copy.jsonl      归档源(可关)
//   projects/<old_key>/memory/...    旧项目 Memory 库
//   projects/<old_key>/project.json  旧 key -> 演示 cwd 的映射
struct LegacyHome {
    fs::path root;
    std::string old_key = "demo-repo-0123456789abcdef";

    explicit LegacyHome(const std::string& name,
                        const std::vector<std::string>& session_names,
                        bool with_archive_copy = false, bool with_memory = true,
                        bool with_project_json = true) {
        root = TempRoot(name);
        const fs::path sessions = root / "sessions";
        std::error_code make_ec;
        fs::create_directories(sessions, make_ec);
        for (const std::string& session_name : session_names) {
            fs::copy_file(FixtureRoot() / "legacy" / (session_name + ".jsonl"),
                          sessions / (LegacySessionId(session_name) + ".jsonl"));
        }
        if (std::find(session_names.begin(), session_names.end(), "mcp-rich-result") !=
            session_names.end()) {
            // rich 块引用的 artifact 字节住 sessions/<id>/mcp-artifacts/(旧布局)。
            CopyTree(FixtureRoot() / "legacy" / "mcp-rich-result.mcp-artifacts",
                     sessions / LegacySessionId("mcp-rich-result") / "mcp-artifacts");
        }
        if (with_archive_copy) {
            // 归档源:sessions/archive/ 下另立一场(改一字节避免与正件同 SHA
            // 撞去重),run 里该件应走 preparing->closed->archived 的加一跳。
            std::string text = ReadAll(FixtureRoot() / "legacy" / "plain-conversation.jsonl");
            text += "{\"type\":\"title\",\"title\":\"archived copy\",\"ts\":\"2026-01-15 10:00:00\"}\n";
            WriteFile(sessions / "archive" / "archived-copy.jsonl", text);
        }
        if (with_memory) {
            CopyTree(FixtureRoot() / "memory" / "project" / "facts",
                     root / "projects" / old_key / "memory" / "facts");
            CopyTree(FixtureRoot() / "memory" / "project" / "feedback",
                     root / "projects" / old_key / "memory" / "feedback");
            CopyTree(FixtureRoot() / "memory" / "project" / "preferences",
                     root / "projects" / old_key / "memory" / "preferences");
            CopyTree(FixtureRoot() / "memory" / "project" / "memory-candidates",
                     root / "projects" / old_key / "memory-candidates");
            if (with_project_json) {
                WriteFile(root / "projects" / old_key / "project.json",
                          nlohmann::json{{"project_root", kDemoCwd}}.dump(2) + "\n");
            }
        }
    }
};

migrator::MigratorOptions OptionsFor(const fs::path& home) {
    migrator::MigratorOptions options;
    options.home_lubancode = home;
    options.lubancode_version = "storage-migrator-test";
    options.now_ms = 1767225600000;  // 固定钟:回执时间戳可断言
    return options;
}

const migrator::MigrationResultItem* FindByPath(const migrator::MigrationRunReport& report,
                                                const std::string& suffix) {
    for (const auto& item : report.items) {
        if (item.source_path.find(suffix) != std::string::npos) {
            return &item;
        }
    }
    return nullptr;
}

// 目标 workspace 根:由夹具 cwd 裁决(不存在路径走 cwd_fallback,key 动态算,
// 不在测试里硬编码)。
std::string DemoWorkspaceKey(const fs::path& home) {
    const auto identity = workspace::ResolveWorkspaceIdentity(platform::Utf8ToPath(kDemoCwd), home);
    REQUIRE(identity.has_value());
    return identity->workspace_key;
}

std::size_t CountImportedSessions(const migrator::MigrationRunReport& report) {
    std::size_t imported = 0;
    for (const auto& item : report.items) {
        if (item.outcome == "imported" || item.outcome == "already_imported") {
            imported += 1;
        }
    }
    return imported;
}

// counts 是增量插入的 map:没发生过的类目没有键,读法一律 0 兜底。
std::size_t CountOf(const migrator::MigrationRunReport& report, const char* key) {
    const auto it = report.counts.find(key);
    return it == report.counts.end() ? 0 : it->second;
}

// 逐件复验目标场:verify + exact replay(P0-5 验收线"成功目标全部可 Replay")。
void CheckTargetsReplayable(const fs::path& home, const migrator::MigrationRunReport& report) {
    for (const auto& item : report.items) {
        if (item.outcome != "imported" && item.outcome != "already_imported") {
            continue;
        }
        const fs::path session_dir = home / "workspaces" /
                                     platform::Utf8ToPath(item.target_workspace_key) / "sessions" /
                                     platform::Utf8ToPath(item.target_session_id);
        const auto verify = trajectory::VerifySessionDir(session_dir);
        if (!verify.ok) {
            std::printf("  [verify-fail] %s: %s (%s)\n", item.target_session_id.c_str(),
                        verify.error_code.c_str(), verify.message.c_str());
        }
        REQUIRE(verify.ok);
        const auto replay = trajectory::FoldStreamReplay(session_dir / "main.jsonl");
        REQUIRE(replay.ok());
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. plan
// ---------------------------------------------------------------------------

TEST_CASE("migrator plan:列源档与 memory 库,intent 落盘,只读不动源") {
    LegacyHome home("plan", {"plain-conversation", "tool-roundtrip", "mcp-rich-result",
                             "compact", "resume", "subagent-foreground", "subagent-background",
                             "linked-worktree"},
                    /*with_archive_copy=*/true);
    const auto before = HashTree(home.root / "sessions");
    REQUIRE_FALSE(before.empty());

    const auto plan = migrator::PlanStorageMigration(OptionsFor(home.root));
    REQUIRE(plan.has_value());
    CHECK_FALSE(plan->operation_id.empty());
    CHECK(plan->errors.empty());
    // 九件源:八份正件 + 一份 archive 拷贝。
    REQUIRE(plan->sessions.size() == 9);
    bool saw_archive = false;
    for (const auto& session : plan->sessions) {
        CHECK(session.source.bytes > 0);
        CHECK(session.source.sha256.size() == 64);
        CHECK(session.source.meta_cwd == kDemoCwd);
        CHECK_FALSE(session.workspace_key.empty());
        CHECK_FALSE(session.already_imported);
        saw_archive = saw_archive || session.archived;
    }
    CHECK(saw_archive);
    // 全部同 cwd -> 同一目标 workspace。
    for (const auto& session : plan->sessions) {
        CHECK(session.workspace_key == plan->sessions.front().workspace_key);
    }
    // memory 侧:一件旧库,mapping 由 project.json 裁决。
    REQUIRE(plan->memory_projects.size() == 1);
    CHECK(plan->memory_projects[0].old_project_key == home.old_key);
    CHECK(plan->memory_projects[0].workspace_key == plan->sessions.front().workspace_key);
    CHECK(plan->memory_projects[0].mapping_source == "project.json");

    // intent.json 落盘且带合同 schema 头;progress 起笔 planned。
    const fs::path operation_dir =
        migrator::receipts::OperationsRoot(home.root) / platform::Utf8ToPath(plan->operation_id);
    const auto intent = migrator::receipts::ReadIntent(operation_dir);
    REQUIRE(intent.has_value());
    CHECK((*intent)["schema"] == "lubancode.storage-migration");
    CHECK((*intent)["version"] == 1);
    CHECK((*intent)["source"]["files"].size() == 9);
    const auto progress = migrator::receipts::ReadProgress(operation_dir);
    REQUIRE(progress.has_value());
    CHECK((*progress)["phase"] == "planned");

    // plan 只读:旧源逐字节不变,workspaces 不曾被建。
    CHECK(HashTree(home.root / "sessions") == before);
    std::error_code ec;
    CHECK_FALSE(fs::exists(home.root / "workspaces", ec));
}

// ---------------------------------------------------------------------------
// 2. run
// ---------------------------------------------------------------------------

TEST_CASE("migrator run:九场全 imported,目标 verify+replay 过,合同键齐") {
    LegacyHome home("run", {"plain-conversation", "tool-roundtrip", "mcp-rich-result",
                            "compact", "resume", "subagent-foreground", "subagent-background",
                            "linked-worktree"},
                    /*with_archive_copy=*/true);
    const auto plan = migrator::PlanStorageMigration(OptionsFor(home.root));
    REQUIRE(plan.has_value());

    const auto run = migrator::RunStorageMigration(OptionsFor(home.root), plan->operation_id);
    REQUIRE(run.has_value());
    CHECK(run->error_code.empty());
    CHECK(run->operation_id == plan->operation_id);
    REQUIRE(run->items.size() == 9);
    for (const auto& item : run->items) {
        if (item.outcome != "imported") {
            std::printf("  [item] %s -> %s: %s (%s)\n", item.source_path.c_str(),
                        item.outcome.c_str(), item.error_code.c_str(),
                        item.missing.empty() ? "" : item.missing.front().c_str());
        }
    }
    CHECK(CountImportedSessions(*run) == 9);
    CHECK(CountOf(*run, "failed") == 0);
    CHECK(CountOf(*run, "skipped_unreadable") == 0);

    // 逐件合同:旧 id 原样带入、terminal hash 非空、训练策略与子账口径。
    for (const auto& item : run->items) {
        CHECK(item.outcome == "imported");
        CHECK(item.terminal_event_hash.size() == 64);
        CHECK(item.subagent_detail == "unavailable_legacy");
        CHECK(item.target_workspace_key == DemoWorkspaceKey(home.root));
        const fs::path name = platform::Utf8ToPath(item.source_path);
        CHECK(item.target_session_id == platform::PathToUtf8(name.stem()));
    }

    // result.json 只许一份(committed 回执)。
    const fs::path operation_dir =
        migrator::receipts::OperationsRoot(home.root) / platform::Utf8ToPath(plan->operation_id);
    const auto result = migrator::receipts::ReadResult(operation_dir);
    REQUIRE(result.has_value());
    CHECK((*result)["counts"]["imported"] == 9);
    CHECK((*result)["training"] == "exclude");
    CHECK((*result)["source_deleted"] == false);

    // 目标场 manifest:legacy_import 场的三把合同键。
    const fs::path session_dir = home.root / "workspaces" /
                                 platform::Utf8ToPath(DemoWorkspaceKey(home.root)) / "sessions" /
                                 LegacySessionId("plain-conversation");
    const auto manifest = trajectory::ReadSessionJson(session_dir);
    REQUIRE(manifest.has_value());
    CHECK(manifest->start_reason == "legacy_import");
    CHECK(manifest->subagent_detail == "unavailable_legacy");
    CHECK(manifest->training_policy == "exclude");
    CHECK(manifest->status == "closed");
    // 归档源加一跳。
    const auto archived = trajectory::ReadSessionJson(session_dir.parent_path() / "archived-copy");
    REQUIRE(archived.has_value());
    CHECK(archived->status == "archived");

    // rich 块:夹具 png 的真实 sha 与 jsonl 里占位 sha 不合,照实列缺口
    //(不冒充可取),该场 legacy_partial。
    const auto* rich = FindByPath(*run, LegacySessionId("mcp-rich-result") + ".jsonl");
    REQUIRE(rich != nullptr);
    CHECK(rich->legacy_partial);
    bool hash_gap_listed = false;
    for (const std::string& gap : rich->missing) {
        hash_gap_listed = hash_gap_listed || gap.find("artifact hash 不合") != std::string::npos;
    }
    CHECK(hash_gap_listed);

    // 目标全部可 replay。
    CheckTargetsReplayable(home.root, *run);

    // memory 侧:三主题迁进 <workspace>/memory/,schema 3,source_sessions
    // 升四段引用;manifest 登记 migrated_from;候选箱跟搬。
    const fs::path workspace_dir = home.root / "workspaces" /
                                   platform::Utf8ToPath(DemoWorkspaceKey(home.root));
    REQUIRE(run->memory_projects.size() == 1);
    const auto& memory_project = run->memory_projects[0];
    CHECK(memory_project.outcome == "imported");
    REQUIRE(memory_project.topics.size() == 3);
    std::size_t upgraded_refs = 0;
    for (const auto& topic : memory_project.topics) {
        CHECK(topic.outcome == "imported");
        CHECK(topic.source_sha256.size() == 64);
        CHECK(topic.target_sha256.size() == 64);
        const fs::path target = workspace_dir / platform::Utf8ToPath(topic.target_file);
        const auto parsed = memory::frontmatter::Parse(ReadAll(target));
        REQUIRE(parsed.has_value());
        CHECK(parsed->entry.schema == 3);
        for (const std::string& ref : parsed->entry.source_sessions) {
            if (ref.find("workspace_key=") != std::string::npos) {
                upgraded_refs += 1;
            }
        }
    }
    CHECK(upgraded_refs > 0);
    std::error_code ec;
    CHECK(fs::exists(workspace_dir / "memory" / "memory-candidates" / "cand.fact.hatch-path-001.json", ec));
    const auto workspace_manifest = workspace::ReadWorkspaceManifest(workspace_dir);
    REQUIRE(workspace_manifest.status == workspace::ManifestRead::Status::Ok);
    REQUIRE(workspace_manifest.manifest.migrated_from.has_value());
    CHECK((*workspace_manifest.manifest.migrated_from)["old_project_key"] == home.old_key);

    // 迁移器只写新账:旧源一字不动。
    CHECK(fs::exists(home.root / "sessions" / (LegacySessionId("plain-conversation") + ".jsonl"), ec));
    CHECK(fs::exists(home.root / "projects" / platform::Utf8ToPath(home.old_key) / "memory" / "facts" /
                          "fact.build-cmd-001.md",
                     ec));
}

// ---------------------------------------------------------------------------
// 3+4. 幂等
// ---------------------------------------------------------------------------

TEST_CASE("migrator 幂等:同源重跑 already_imported,不再造第二份") {
    LegacyHome home("idempotent", {"plain-conversation", "tool-roundtrip", "compact"});
    const auto plan = migrator::PlanStorageMigration(OptionsFor(home.root));
    REQUIRE(plan.has_value());
    const auto first = migrator::RunStorageMigration(OptionsFor(home.root), plan->operation_id);
    REQUIRE(first.has_value());
    REQUIRE(first->counts.at("imported") == 3);

    const fs::path sessions_root = home.root / "workspaces" /
                                   platform::Utf8ToPath(DemoWorkspaceKey(home.root)) / "sessions";
    std::size_t sessions_after_first = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(sessions_root, ec)) {
        if (entry.is_directory()) {
            sessions_after_first += 1;
        }
    }
    CHECK(sessions_after_first == 3);

    // 第二只 operation:同 SHA 全部 already_imported,目标不增。
    const auto plan2 = migrator::PlanStorageMigration(OptionsFor(home.root));
    REQUIRE(plan2.has_value());
    CHECK(plan2->imported_before == 3);
    for (const auto& session : plan2->sessions) {
        CHECK(session.already_imported);
    }
    const auto second = migrator::RunStorageMigration(OptionsFor(home.root), plan2->operation_id);
    REQUIRE(second.has_value());
    CHECK(second->counts.count("imported") == 0);
    CHECK(second->counts.at("already_imported") == 3);
    // already_imported 逐件回指首迁的目标。
    for (const auto& item : second->items) {
        CHECK(item.outcome == "already_imported");
        CHECK_FALSE(item.target_session_id.empty());
        CHECK_FALSE(item.target_workspace_key.empty());
    }
    std::size_t sessions_after_second = 0;
    for (const auto& entry : fs::directory_iterator(sessions_root, ec)) {
        if (entry.is_directory()) {
            sessions_after_second += 1;
        }
    }
    CHECK(sessions_after_second == 3);
    // memory 主题按字节相同判 already_imported。
    REQUIRE(second->memory_projects.size() == 1);
    for (const auto& topic : second->memory_projects[0].topics) {
        CHECK(topic.outcome == "already_imported");
    }
    CheckTargetsReplayable(home.root, *second);
}

TEST_CASE("migrator:committed operation 拒重跑(migration.result_exists)") {
    LegacyHome home("result-exists", {"plain-conversation"});
    const auto plan = migrator::PlanStorageMigration(OptionsFor(home.root));
    REQUIRE(plan.has_value());
    const auto first = migrator::RunStorageMigration(OptionsFor(home.root), plan->operation_id);
    REQUIRE(first.has_value());
    const auto again = migrator::RunStorageMigration(OptionsFor(home.root), plan->operation_id);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().find("migration.result_exists") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 5. sha_mismatch
// ---------------------------------------------------------------------------

TEST_CASE("migrator:plan 后源被改,该件 source_sha_mismatch,整批不中断") {
    LegacyHome home("sha-mismatch", {"plain-conversation", "tool-roundtrip", "compact"});
    const auto plan = migrator::PlanStorageMigration(OptionsFor(home.root));
    REQUIRE(plan.has_value());
    // intent 冻结后动一件源的字节。
    const fs::path victim = home.root / "sessions" / (LegacySessionId("tool-roundtrip") + ".jsonl");
    std::string text = ReadAll(victim);
    text += "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"late edit\"}],"
            "\"ts\":\"2026-01-16 11:00:00\"}\n";
    WriteFile(victim, text);

    const auto run = migrator::RunStorageMigration(OptionsFor(home.root), plan->operation_id);
    if (!run.has_value()) {
        std::printf("  [run-error] %s\n", run.error().c_str());
    }
    REQUIRE(run.has_value());
    const auto* changed = FindByPath(*run, LegacySessionId("tool-roundtrip") + ".jsonl");
    REQUIRE(changed != nullptr);
    CHECK(changed->outcome == "failed");
    CHECK(changed->error_code == "migration.source_sha_mismatch");
    // 其余两件照常 imported:整批不因单件失败中断。
    CHECK(run->counts.at("imported") == 2);
    CHECK(run->counts.at("failed") == 1);
    CheckTargetsReplayable(home.root, *run);
}

// ---------------------------------------------------------------------------
// 6. delete-source
// ---------------------------------------------------------------------------

TEST_CASE("migrator delete-source:无 --yes 拒;有 --yes 只删已核验源") {
    LegacyHome home("delete", {"plain-conversation", "compact"});
    const auto plan = migrator::PlanStorageMigration(OptionsFor(home.root));
    REQUIRE(plan.has_value());

    auto options = OptionsFor(home.root);
    options.delete_source = true;
    options.confirm_delete = false;
    const auto refused = migrator::RunStorageMigration(options, plan->operation_id);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().find("migration.delete_unverified") != std::string::npos);
    // 拒删时源一字未少,也没有半截 result。
    std::error_code ec;
    CHECK(fs::exists(home.root / "sessions" / (LegacySessionId("plain-conversation") + ".jsonl"), ec));
    CHECK_FALSE(migrator::receipts::ReadResult(
                    migrator::receipts::OperationsRoot(home.root) /
                    platform::Utf8ToPath(plan->operation_id))
                    .has_value());

    // 带二次确认:三件源(两场会话+memory 库不动)按合同删会话源档。
    options.confirm_delete = true;
    const auto run = migrator::RunStorageMigration(options, plan->operation_id);
    REQUIRE(run.has_value());
    CHECK(run->source_deleted);
    REQUIRE(run->deleted_sources.size() == 2);
    CHECK_FALSE(fs::exists(home.root / "sessions" / (LegacySessionId("plain-conversation") + ".jsonl"), ec));
    CHECK_FALSE(fs::exists(home.root / "sessions" / (LegacySessionId("compact") + ".jsonl"), ec));
    // 旧 memory 库不属 --delete-source 的删除面(只删会话源档)。
    CHECK(fs::exists(home.root / "projects" / platform::Utf8ToPath(home.old_key) / "memory" / "facts" /
                          "fact.build-cmd-001.md",
                     ec));
    // 目标与回执都在,且仍可 replay。
    CheckTargetsReplayable(home.root, *run);
    const auto result = migrator::receipts::ReadResult(
        migrator::receipts::OperationsRoot(home.root) / platform::Utf8ToPath(plan->operation_id));
    REQUIRE(result.has_value());
    CHECK((*result)["source_deleted"] == true);
}

// ---------------------------------------------------------------------------
// 7. memory unmappable
// ---------------------------------------------------------------------------

TEST_CASE("migrator:算不出目标的旧 memory 库列账 unmappable,--project-root 可解") {
    LegacyHome home("unmappable", {"plain-conversation"},
                    /*with_archive_copy=*/false,
                    /*with_memory=*/true, /*with_project_json=*/false);
    const auto plan = migrator::PlanStorageMigration(OptionsFor(home.root));
    REQUIRE(plan.has_value());
    REQUIRE(plan->memory_projects.size() == 1);
    CHECK(plan->memory_projects[0].outcome == "unmappable");
    CHECK(plan->memory_projects[0].workspace_key.empty());

    const auto run = migrator::RunStorageMigration(OptionsFor(home.root), plan->operation_id);
    if (!run.has_value()) {
        std::printf("  [run-error unmappable] %s\n", run.error().c_str());
    }
    REQUIRE(run.has_value());
    REQUIRE(run->memory_projects.size() == 1);
    CHECK(run->memory_projects[0].outcome == "unmappable");
    CHECK(run->memory_projects[0].note.find("--project-root") != std::string::npos);
    // 会话侧不受牵连。
    CHECK(run->counts.at("imported") == 1);

    // status 侧:unmappable 进专列,不冒充 pending。
    const auto status = migrator::QueryStorageMigrationStatus(OptionsFor(home.root));
    REQUIRE(status.unmappable_projects.size() == 1);
    CHECK(status.unmappable_projects[0] == home.old_key);
    CHECK(status.pending_memory_projects == 0);

    // 显式映射后重跑:主题补迁。
    auto options = OptionsFor(home.root);
    options.extra_project_roots[home.old_key] = kDemoCwd;
    const auto plan2 = migrator::PlanStorageMigration(options);
    REQUIRE(plan2.has_value());
    const auto run2 = migrator::RunStorageMigration(options, plan2->operation_id);
    REQUIRE(run2.has_value());
    REQUIRE(run2->memory_projects.size() == 1);
    CHECK(run2->memory_projects[0].outcome == "imported");
    CHECK(run2->memory_projects[0].workspace_key == DemoWorkspaceKey(home.root));
}

// ---------------------------------------------------------------------------
// 8. status
// ---------------------------------------------------------------------------

TEST_CASE("migrator status:pending/committed/unmappable 三本账") {
    LegacyHome home("status", {"plain-conversation", "tool-roundtrip"},
                    /*with_archive_copy=*/false, /*with_memory=*/true,
                    /*with_project_json=*/false);
    // 再塞一只无 memory 的旧项目目录:残留登记,不算 pending。
    std::error_code ec;
    fs::create_directories(home.root / "projects" / "empty-residue", ec);

    const auto early = migrator::QueryStorageMigrationStatus(OptionsFor(home.root));
    CHECK(early.operations.empty());
    CHECK(early.committed_operations == 0);
    CHECK(early.pending_session_files == 2);
    CHECK(early.pending_memory_projects == 0);  // unmappable 走专列
    REQUIRE(early.unmappable_projects.size() == 1);

    auto options = OptionsFor(home.root);
    options.extra_project_roots[home.old_key] = kDemoCwd;
    const auto plan = migrator::PlanStorageMigration(options);
    REQUIRE(plan.has_value());
    const auto mid = migrator::QueryStorageMigrationStatus(options);
    REQUIRE(mid.operations.size() == 1);
    CHECK(mid.operations[0].phase == "planned");

    const auto run = migrator::RunStorageMigration(options, plan->operation_id);
    REQUIRE(run.has_value());
    const auto late = migrator::QueryStorageMigrationStatus(options);
    CHECK(late.committed_operations == 1);
    // 源档未删,但 SHA 已 committed:不再算 pending(§7.3"未迁清单")。
    CHECK(late.pending_session_files == 0);
    CHECK(late.pending_memory_projects == 0);
    REQUIRE(late.operations.size() == 1);
    CHECK(late.operations[0].phase == "committed");
    CHECK(late.operations[0].total == 2);
}

// ---------------------------------------------------------------------------
// 9. fault hook:打断一百次(核心验收线)
// ---------------------------------------------------------------------------

TEST_CASE("migrator 验收:故障注入打断一百次,旧源无损,续跑不重样,终局全过") {
    // 六场 + memory 库:耐久点(event_committed 每事件一发,加七种点)
    // 总量过百,足够逐点打断一百轮。
    LegacyHome home("fault100", {"plain-conversation", "tool-roundtrip", "mcp-rich-result",
                                 "compact", "resume", "subagent-foreground"});
    const auto sources_before = HashTree(home.root / "sessions");
    const auto memory_before = HashTree(home.root / "projects");
    REQUIRE(sources_before.size() == 7);  // 六场 + 一枚 artifact png

    const auto plan = migrator::PlanStorageMigration(OptionsFor(home.root));
    REQUIRE(plan.has_value());

    // 第 target 次越过耐久点即打断;每轮 target 递增,即前一百个耐久点
    // 逐点各打断一次(与"任意时刻崩溃"等价的系统性覆盖)。
    int fault_target = 0;
    int interruptions = 0;
    int fault_points_seen = 0;
    auto options = OptionsFor(home.root);
    options.fault = [&fault_target, &interruptions, &fault_points_seen](const std::string& point) {
        (void)point;
        ++fault_points_seen;
        if (fault_points_seen == fault_target) {
            ++interruptions;
            return true;
        }
        return false;
    };

    int rounds = 0;
    std::optional<migrator::MigrationRunReport> final_run;
    for (; rounds < 250; ++rounds) {
        fault_target = rounds + 1;
        fault_points_seen = 0;
        const auto run = migrator::RunStorageMigration(options, plan->operation_id);
        if (!run.has_value()) {
            std::printf("  [run-error fault100] %s\n", run.error().c_str());
        }
        REQUIRE(run.has_value());
        if (run->error_code == "migration.interrupted") {
            // 每轮打断后先验旧源无损,再续跑。
            CHECK(HashTree(home.root / "sessions") == sources_before);
            CHECK(HashTree(home.root / "projects") == memory_before);
            CHECK_FALSE(run->error_text.empty());
            continue;
        }
        CHECK(run->error_code.empty());
        final_run = *run;
        break;
    }
    // 打满一百次后第 101 轮收工(若耐久点先耗尽则提前成,如实对账)。
    CHECK(interruptions >= 100);
    CHECK(rounds == interruptions);
    REQUIRE(final_run.has_value());

    // 续跑不重样:六场每 SHA 恰一个目标,合计 imported+already==6。
    CHECK(CountImportedSessions(*final_run) == 6);
    std::map<std::string, std::string> sha_to_target;
    for (const auto& item : final_run->items) {
        if (item.outcome != "imported" && item.outcome != "already_imported") {
            continue;
        }
        const std::string target = item.target_workspace_key + "/" + item.target_session_id;
        const auto [it, inserted] = sha_to_target.emplace(item.source_sha256, target);
        CHECK(inserted);
    }
    CHECK(sha_to_target.size() == 6);
    // 目标场目录数与源件数一一对应。
    const fs::path sessions_root = home.root / "workspaces" /
                                   platform::Utf8ToPath(DemoWorkspaceKey(home.root)) / "sessions";
    std::size_t target_dirs = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(sessions_root, ec)) {
        if (entry.is_directory()) {
            target_dirs += 1;
        }
    }
    CHECK(target_dirs == 6);

    // 旧源终局仍无损(全程未开 delete_source)。
    CHECK(HashTree(home.root / "sessions") == sources_before);
    CHECK(HashTree(home.root / "projects") == memory_before);
    // 最终 result committed,目标 verify+replay 全过。
    CheckTargetsReplayable(home.root, *final_run);
    const auto result = migrator::receipts::ReadResult(
        migrator::receipts::OperationsRoot(home.root) / platform::Utf8ToPath(plan->operation_id));
    REQUIRE(result.has_value());
    // memory 也收口:主题全部到位。
    REQUIRE(final_run->memory_projects.size() == 1);
    std::size_t topics_ok = 0;
    for (const auto& topic : final_run->memory_projects[0].topics) {
        if (topic.outcome == "imported" || topic.outcome == "already_imported") {
            topics_ok += 1;
        }
    }
    CHECK(topics_ok == 3);
}

// ---------------------------------------------------------------------------
// 10. 真进程 kill 折算
// ---------------------------------------------------------------------------

namespace {

std::string FindLegacyMigratorBinary() {
#ifdef LUBANCODE_BINARY_DIR
    std::error_code ec;
    for (const char* name : {"legacy-storage-migrator", "legacy-storage-migrator.exe",
                             "Release/legacy-storage-migrator.exe",
                             "Debug/legacy-storage-migrator.exe"}) {
        const fs::path candidate = fs::path(LUBANCODE_BINARY_DIR) / name;
        if (fs::exists(candidate, ec)) {
            return platform::PathToUtf8(candidate);
        }
    }
#endif
    return std::string();
}

}  // namespace

TEST_CASE("migrator 验收:真进程强杀折算,续跑到 committed,如实记录") {
    const std::string binary = FindLegacyMigratorBinary();
    if (binary.empty()) {
        return;  // 缺独立封存体(只编了 lubancode_tests 的构建):跳过
    }
    LegacyHome home("kill", {"plain-conversation", "tool-roundtrip", "mcp-rich-result",
                             "compact", "resume", "subagent-foreground", "subagent-background",
                             "linked-worktree"});
    const auto sources_before = HashTree(home.root / "sessions");
    const auto memory_before = HashTree(home.root / "projects");

    // 起一只 plan,让 kill 循环全部落在"续跑同一 operation"的路径上。
    const auto plan = migrator::PlanStorageMigration(OptionsFor(home.root));
    REQUIRE(plan.has_value());
    const fs::path operation_dir =
        migrator::receipts::OperationsRoot(home.root) / platform::Utf8ToPath(plan->operation_id);

    // 15 轮折算:随机延迟后强杀(TerminateProcess 口径,不给体面退出的
    // 机会),记录杀时回执状态与"杀时进程是否还活着"(不活着 = 进程跑得
    // 比刀快,那轮不算真打断,如实归账)。
    std::mt19937 rng(20260901);
    int kills = 0;
    int kills_while_running = 0;  // 刀落下时进程还在跑(真打断)
    int kills_before_commit = 0;  // 杀完回执仍未 committed(含进程已自退)
    int kills_after_commit = 0;   // 杀完回执已 committed(跑赢了刀)
    for (int round = 0; round < 15; ++round) {
        platform::ChildProcess child;
        std::string stdout_text;
        const auto started = child.Start(
            binary, {"run", "--home", platform::PathToUtf8(home.root),
                     "--operation", plan->operation_id},
            /*env=*/{},
            [&stdout_text](std::string_view chunk) {
                stdout_text.append(chunk);
                return true;
            },
            [](std::string_view) {});
        REQUIRE(started.success);
        const int delay_ms = 5 + static_cast<int>(rng() % 60);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        if (child.IsAlive()) {
            ++kills_while_running;
        }
        child.Kill();
        child.WaitForExit(5000);
        const bool committed_at_kill =
            migrator::receipts::ReadResult(operation_dir).has_value();
        ++kills;
        (committed_at_kill ? kills_after_commit : kills_before_commit) += 1;
        // 每轮杀完旧源都必须无损。
        CHECK(HashTree(home.root / "sessions") == sources_before);
        CHECK(HashTree(home.root / "projects") == memory_before);
        if (committed_at_kill) {
            break;  // 已 committed:后续轮次只剩空转,不再折算
        }
    }
    REQUIRE(kills <= 15);

    // 终局:再起一次跑到体面退出,断言 committed 且目标全过复验。
    platform::ChildProcess finisher;
    std::string stdout_text;
    const auto started = finisher.Start(
        binary, {"run", "--home", platform::PathToUtf8(home.root),
                 "--operation", plan->operation_id},
        /*env=*/{},
        [&stdout_text](std::string_view chunk) {
            stdout_text.append(chunk);
            return true;
        },
        [](std::string_view) {});
    REQUIRE(started.success);
    REQUIRE(finisher.WaitForExit(60000));
    finisher.Shutdown(1000);  // 收尸后才读得出退出码
    if (finisher.exit_code() != 0) {
        std::printf("  [finisher] exit=%d stdout=%s\n", finisher.exit_code(),
                    stdout_text.c_str());
    }
    CHECK(finisher.exit_code() == 0);

    const auto result = migrator::receipts::ReadResult(operation_dir);
    REQUIRE(result.has_value());
    CHECK((*result)["counts"]["imported"] == 8);
    CHECK(HashTree(home.root / "sessions") == sources_before);
    CHECK(HashTree(home.root / "projects") == memory_before);

    // 借 result 对账目标(不重复实现 RunReport 的读法)。
    migrator::MigrationRunReport report;
    report.operation_id = plan->operation_id;
    for (const auto& item : (*result)["items"]) {
        migrator::MigrationResultItem entry;
        entry.source_sha256 = item.value("source_sha256", std::string());
        entry.source_path = item.value("source_path", std::string());
        entry.outcome = item.value("outcome", std::string());
        entry.target_session_id = item.value("target_session_id", std::string());
        entry.target_workspace_key = item.value("target_workspace_key", std::string());
        report.items.push_back(std::move(entry));
    }
    CheckTargetsReplayable(home.root, report);

    // 如实记录(ctest 静默时看 -s 或失败输出):
    //   折算总轮数 / 刀落下时进程仍在跑(真打断) / 杀时已 committed(跑赢刀)。
    std::printf("[kill-fold] rounds=%d killed-while-running=%d interrupted-before-commit=%d "
                "already-committed=%d\n",
                kills, kills_while_running, kills_before_commit, kills_after_commit);
}
