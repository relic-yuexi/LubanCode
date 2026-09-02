// 存储 v2 一次性迁移器(P0-5):把旧格式用户数据迁进 workspaces/ 新账。
//
// 《Workspace 统一存储、旧 Session 退场与分级 Memory 迁移》§7.1/§7.3 与
// docs/development/workspace-storage-v2/P0-0-contracts.md §五(迁移回执
// schema 冻结,照办不改)。规矩:
//   - 只读旧档写新账:旧源(~/.lubancode/sessions、~/.lubancode/projects)
//     默认一字不动;--delete-source 且二次确认、只删已 committed 且复验
//     过的源档(§7.1 第 8 条)。
//   - 幂等:同一 source SHA 已 committed 即回既有目标,不再造一份
//     (result.json 只许 create-new;migration.result_exists)。
//   - 崩溃续跑:progress.json 每文件一笔原子替换;重跑按 progress/hash
//     续办,半截目标目录(无 committed 回执)删除重建(migration.interrupted)。
//   - 迁移场 session.json 记 start_reason=legacy_import、
//     subagent_detail=unavailable_legacy、training_policy=exclude;
//     不可还原的执行边界照实列进 missing[](legacy_partial=true)。
//
// 隔离边界(单子 §7.4):旧格式行解析只活在 storage_migrator.cpp 内部
// (匿名命名空间),不进任何生产 runtime;收官发行时本目标整体封存或移出
// 生产构建(见 docs/getting-started/storage-migration.md 的版本边界)。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::workspace::migrator {

// ---------------------------------------------------------------------------
// 选项
// ---------------------------------------------------------------------------

struct MigratorOptions {
    // ~/.lubancode(必填;测试注入临时根)。
    std::filesystem::path home_lubancode;
    // 新账根;空 = home_lubancode/workspaces(生产唯一项目持久化根)。
    std::filesystem::path workspaces_root;
    std::string lubancode_version;  // 写进 session.json 的写入方版本
    std::int64_t now_ms = 0;        // 0 = 现取墙钟;测试注固定钟

    // 只列计划不动盘(plan 的语义);run 内部不用。
    bool dry_run = false;

    // 删源合同(§7.1 第 8 条):delete_source 须配 confirm_delete(二次
    // 确认的 --yes),且只删 result 已 committed、当下重算 sha 仍合、目标
    // 复验(verify+replay)通过的源档。任何一环不合即整批不删。
    bool delete_source = false;
    bool confirm_delete = false;

    // 额外的旧项目根映射:projects/ 下没有 project.json 的旧库,靠显式
    // 递路径算旧 key 对账(key -> 项目根,UTF-8 文本)。CLI 的
    // --project-root <path> 落进来。
    std::map<std::string, std::string> extra_project_roots;

    // 故障注入(验收线"打断一百次"):每越过一道耐久点回调一次,回 true
    // 即当场模拟崩溃——迁移器立即中止(migration.interrupted),磁盘停在
    // 该耐久点之后。测试据此反复打断再续跑。point 取值:
    //   "session_created"    目标 session 目录占位后
    //   "event_committed"    每枚事件落账后
    //   "session_closed"     目标场封口后
    //   "file_imported"      单个源文件进 result 前账(progress 更新)后
    //   "memory_topic"       每主题写盘后
    //   "result_committed"   result.json 原子写之后
    //   "source_deleted"     每个源档删除后
    std::function<bool(const std::string& point)> fault;

    // 进度回调(给 CLI 打进度;不回值,不影响流程)。每完成一个源文件或
    // 一个 memory 主题各调一次。
    std::function<void(const std::string& note)> progress_note;
};

// ---------------------------------------------------------------------------
// 回执形状(合同 §五;schema 名/版本常量在 storage_contracts.hpp)
// ---------------------------------------------------------------------------

// intent.json 里的一名源文件。
struct MigrationSourceFile {
    std::string path;      // 相对 home_lubancode(如 sessions/2026...-.jsonl)
    std::uintmax_t bytes = 0;
    std::string sha256;    // hex64
    std::string meta_cwd;  // 旧档首行 meta.cwd(UTF-8 文本)
};

// result.json 里的一名单件结果(合同 §五 items[])。
struct MigrationResultItem {
    std::string source_sha256;
    std::string source_path;
    std::string outcome;             // imported | already_imported | skipped_unreadable | failed
    std::string target_session_id;   // 旧 id 原样带入(§2D)
    std::string target_workspace_key;
    std::string terminal_event_hash; // 目标场 terminal 事件 hash;failed 时空
    bool legacy_partial = false;
    std::string subagent_detail;     // unavailable_legacy(旧场无子账)
    std::vector<std::string> missing;
    std::string error_code;          // failed 时给稳定码
};

// memory 侧逐主题账(§7.3"source/target hash 对照")。
struct MigrationMemoryTopic {
    std::string id;
    std::string source_file;      // 相对旧 memory 根
    std::string source_sha256;    // 旧主题全文 sha256
    std::string target_file;      // 相对新 memory 根
    std::string target_sha256;    // 写成后的全文 sha256(升 schema 3 会变)
    std::string outcome;          // imported | already_imported | skipped | failed
    std::string note;             // skipped/failed 的原因;升级了 source_sessions 也注明
};

// 一只旧项目库的迁移结果。
struct MigrationMemoryProject {
    std::string old_project_key;
    std::string source_dir;        // 相对 home_lubancode(projects/<key>/memory)
    std::string workspace_key;     // 目标 workspace;算不出即空(unmappable)
    std::string mapping_source;    // project.json | project-root 选项 | cwd
    std::vector<MigrationMemoryTopic> topics;
    std::vector<std::string> candidates;  // 迁过去的候选文件名
    std::string outcome;           // imported | partial | skipped | unmappable | failed
    std::string note;
};

// ---------------------------------------------------------------------------
// plan / run / status 三口的报告
// ---------------------------------------------------------------------------

struct MigrationPlanSession {
    MigrationSourceFile source;
    std::string workspace_key;     // meta.cwd 裁决出的目标
    std::string identity_kind;
    bool already_imported = false; // 该 source SHA 已有 committed 目标
    bool archived = false;         // 来自 sessions/archive/
};

struct MigrationPlanReport {
    std::string operation_id;
    std::vector<MigrationPlanSession> sessions;
    std::vector<MigrationMemoryProject> memory_projects;
    std::vector<std::string> errors;  // 扫描期读不出的源(逐条记,不中断)
    std::size_t imported_before = 0;  // 全机已 committed 的源文件数(去重)
};

struct MigrationRunReport {
    std::string operation_id;
    std::string resumed_operation;  // 非空 = 续跑了这只未完 operation
    std::vector<MigrationResultItem> items;
    std::vector<MigrationMemoryProject> memory_projects;
    // counts(合同 §五):imported/already_imported/skipped_unreadable/failed
    std::map<std::string, std::size_t> counts;
    bool source_deleted = false;
    std::vector<std::string> deleted_sources;
    std::string error_code;  // 整批级失败(如 migration.interrupted);空=成
    std::string error_text;
};

struct MigrationOperationStatus {
    std::string operation_id;
    std::string phase;         // planned | importing | committed | unknown
    std::size_t done = 0;
    std::size_t total = 0;
    std::string last_source_sha256;
    std::string last_outcome;
    std::int64_t updated_at_ms = 0;
};

struct MigrationStatusReport {
    std::vector<MigrationOperationStatus> operations;  // 新→旧
    // 尚未迁的旧数据(§7.3"不能因某个旧 project 眼下没打开,就说全机迁完")
    std::size_t pending_session_files = 0;   // 无 committed 目标的旧会话档
    std::size_t pending_memory_projects = 0; // 还有主题未迁的旧项目库
    std::vector<std::string> unmappable_projects;  // 算不出目标的旧 key
    std::size_t committed_operations = 0;
};

// ---------------------------------------------------------------------------
// 三口
// ---------------------------------------------------------------------------

// 扫旧源、写 intent.json(create-new;复用 operation_id 报
// migration.intent_exists)。只读旧档 + 建回执目录,不动任何源字节。
std::expected<MigrationPlanReport, std::string> PlanStorageMigration(const MigratorOptions& options);

// 执行迁移。operation_id 空 = 自动续跑最近一只未 committed 的 operation
// (没有则现 plan 一只);非空 = 指认续跑。逐文件幂等:
//   - source SHA 已有 committed 目标 -> already_imported;
//   - progress 已记账的文件不重做(重开时按 result 对账);
//   - 半截目标 session(无 committed 回执)删了重建。
// 全部文件与 memory 处置完原子写 result.json(只有它算 committed)。
// 删源只在 options 合同满足且逐件复验通过时发生。
std::expected<MigrationRunReport, std::string> RunStorageMigration(const MigratorOptions& options,
                                                                   const std::string& operation_id = "");

// 只读账面:列 operations 进度与全机未迁清单。不读旧档正文(只看目录
// 与回执),零写盘。
MigrationStatusReport QueryStorageMigrationStatus(const MigratorOptions& options);

// ---------------------------------------------------------------------------
// 迁移回执的读写(测试与 status 共用;错误文本带稳定码前缀)
// ---------------------------------------------------------------------------

namespace receipts {

std::filesystem::path OperationsRoot(const std::filesystem::path& home_lubancode);

// 读一只 operation 的 intent/progress/result;missing 文件给 nullopt。
std::optional<nlohmann::json> ReadIntent(const std::filesystem::path& operation_dir);
std::optional<nlohmann::json> ReadProgress(const std::filesystem::path& operation_dir);
std::optional<nlohmann::json> ReadResult(const std::filesystem::path& operation_dir);

}  // namespace receipts

// ---------------------------------------------------------------------------
// 旧 key 复算(§7.3:由旧 ResolveProjectIdentity 只算一次;算法照
// P0-1 收编前的原样,封死在迁移器里,生产不再持有)
// ---------------------------------------------------------------------------

// 旧 project_key:<safe(display_name,48)>-<16hex FNV-1a 64(seed)>。
// seed = ("git:"|"path:") + 规范绝对 common_root(generic 分隔符,
// Windows 折叠 ASCII 小写)。display_name 取 common git dir 父目录名
// (linked worktree 不裂)。
std::string ComputeLegacyProjectKey(const std::filesystem::path& project_root);

}  // namespace lubancode::workspace::migrator
