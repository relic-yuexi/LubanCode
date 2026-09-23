// /evolve 命令(自进化闭环阶段 1/2/3/4):status/list/show 只读面,阶段 2 的
// propose/diff/reject,阶段 3 的 test,阶段 4 的 approve/use/promote/rollback
// ——批准页、点名 canary、晋升与回滚。候选状态机的写笔全在
// EvolutionCoordinator(契约铁律),命令层只递材料、只打印,不自写迁移。
#pragma once

#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "app/cli_options.hpp"            // EvolveTestArgs(CI 子命令)
#include "cli/slash_commands.hpp"          // ParsedSlashCommand(分派注册制)
#include "cli/theme.hpp"                   // Theme(TUI 排版批 5c 渲染段)

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lubancode::memory {
class ProjectMemory;  // /evolve 的分层账(指针借用,定义在 memory/project_memory.hpp)
}  // namespace lubancode::memory

namespace lubancode::app {

enum class EvolveCommandAction {
    Invalid,
    Status,   // /evolve status(裸 /evolve 同义):扫五路账本采观察,报账面
    List,     // /evolve list [run|goal|recording|tooltrace|memory|all]:按指纹聚类列账 + 候选区
    Show,     // /evolve show <观察id|候选id>:看一条观察或一只候选,指回来源
    Propose,  // /evolve propose <recording-id|observation-id>:起草并落候选(阶段 2)
    Diff,     // /evolve diff <candidate-id>:与父版或空对照(阶段 2)
    Reject,   // /evolve reject <candidate-id> [reason]:落 rejected,指纹进拒绝账(阶段 2)
    Test,     // /evolve test <candidate-id>:跑评测五道门,账只追加(阶段 3)
    Approve,  // /evolve approve <candidate-id>:出批准页并装 store(阶段 4)
    Use,      // /evolve use <candidate-id>:点名 canary(阶段 4)
    Promote,  // /evolve promote <candidate-id>:canary -> active(阶段 4)
    Rollback, // /evolve rollback <package-id> [version]:切回父版或指定版(阶段 4)
    Suggest,  // /evolve suggest [on|off]:看/开关有限自动建议(阶段 7,缺省关)
};

struct ParsedEvolveCommand {
    EvolveCommandAction action = EvolveCommandAction::Invalid;
    std::string source_filter;  // List 时:run/goal/recording/tooltrace/memory;nullopt 语义用空串 = all
    std::string target;         // Show/Propose/Diff/Reject/Test/Approve/Use/Promote/Rollback 的目标 id
    std::string target_extra;   // Rollback 的可选版本号
    std::string reason;         // Reject 的理由(可省)
    std::string suggest_arg;    // Suggest 时:on / off;空串 = 看状态与门槛账
    std::string bad_word;       // Invalid 时第一词的原始拼写(提示用)
};

// 纯解析:拆出动作、来源过滤、目标。认不得的子命令、show 缺 id 一律
// Invalid,由 handler 统一打用法。
ParsedEvolveCommand ParseEvolveCommand(const std::string& args);

// ---------------- 纯渲染(TUI 排版批 5c:status/list/show/propose) ----------------
// RunEvolve* 装好材料,渲染零 IO;形状册 tests/unit/app/test_evolve_commands_frame.cpp
// 直接钉。文案是既有硬编码中文,一字不添不改(批 2 裁量 1);句内冒号按
// SentenceField 拆两列(批 1 裁量 2),表头用数据 schema 名(批 1 裁量 1)。

// status 的账面材料(RunEvolveStatus 装好)。
struct EvolveStatusModel {
    std::size_t recordings_scanned = 0;
    std::size_t recordings_skipped = 0;
    std::size_t runs_scanned = 0;
    std::size_t memory_entries = 0;
    std::size_t appended = 0;
    std::size_t duplicates = 0;
    std::size_t suppressed = 0;
    std::size_t ledger_size = 0;
    std::size_t cluster_count = 0;
    std::vector<std::pair<std::string, int>> by_source;  // 账面序:source 名 -> 条数
    std::string observations_file;                       // UTF-8
    std::string error;                                   // 非空 = 有观察没落住账
};
std::vector<std::string> FormatEvolveStatusLines(const EvolveStatusModel& model,
                                                 const lubancode::cli::Theme& theme, int width);

// list 的簇行(summary/peers 由调用方截断;peers 空 = 单条簇)。
struct EvolveClusterRow {
    std::string fingerprint;
    std::string count;    // "x3"
    std::string source;   // "recording"
    std::string outcome;  // "success"
    std::string summary;
    std::string peers;    // 同类观察 id 串(旧"同类:"行的值段)
};
std::vector<std::string> FormatEvolveClusterLines(std::size_t ledger_size,
                                                  const std::vector<EvolveClusterRow>& rows,
                                                  const lubancode::cli::Theme& theme, int width);

// list 候选区的行(objective 已截断;可空)。
struct EvolveCandidateRow {
    std::string candidate_id;
    std::string state;
    std::string package_id;
    std::string objective;
};
std::vector<std::string> FormatEvolveCandidateListLines(const std::vector<EvolveCandidateRow>& rows,
                                                        const lubancode::cli::Theme& theme,
                                                        int width);

// show 观察页材料(evidence 为 ref -> note;created_at/details_json 空 = 不显)。
struct EvolveObservationModel {
    std::string id;
    std::string source;
    std::string outcome;
    std::string source_id;
    std::string source_ref;    // 原始账;空 = "(无)"
    std::string fingerprint;
    std::string summary;
    std::string created_at;
    std::string details_json;
    std::vector<std::pair<std::string, std::string>> evidence;
};
std::vector<std::string> FormatEvolveObservationLines(const EvolveObservationModel& model,
                                                      const lubancode::cli::Theme& theme,
                                                      int width);

// show 候选页材料(评测摘要正文由调用方在框外跟出,批 1 裁量 3"长正文不
// 塞框";下一步通知同理在框后)。
struct EvolveCandidatePageModel {
    std::string candidate_id;
    std::string state;
    std::string package_id;
    std::string dir_utf8;
    std::string content_hash;  // 空 = "(package/ 缺失)"
    bool has_shape = false;
    std::string shape;         // 形状句(复杂度已并)
    bool has_record = false;
    std::string candidate_version;  // "1.2(父版 1.1 abc)" / "1.0(无父版,与空对照)"
    std::string objective;           // 已截断
    std::string sources;             // 来源回指串;"(演化账未记来源)"
    std::string generator;           // "provider / model / rev"
    std::string changes;             // "新增组件 … 权限差异 N 条,新工具 M 件"
    std::vector<std::string> tools_added;
    std::vector<std::string> permissions_added;
    std::string created_at;  // "(未记)" 已处理好
    bool has_approval = false;
    std::string approval;    // "tier / status(由 … 决定;指纹 …)"
    bool has_eval = false;
    std::size_t eval_rows = 0;
};
std::vector<std::string> FormatEvolveCandidatePageLines(const EvolveCandidatePageModel& model,
                                                        const lubancode::cli::Theme& theme,
                                                        int width);

// propose 落账页材料(全单行值,一框收口)。
struct EvolveProposeModel {
    std::string candidate;  // "cand-x  [pkg 1.0]"
    std::string content_hash;
    std::string dir_utf8;
    std::string shape;      // 形状句(分档全文)
    std::string rule;       // 草稿规矩(可空 = 不显)
    std::string components; // 组件串(含 content-only/code-bearing 注)
    std::vector<std::string> tools_added;
    std::vector<std::string> permissions_added;
    std::string gate;       // 档位门(可空 = 不显)
    std::string downgrade;  // 降档注(可空 = 不显)
    int cluster_skipped = 0;
    std::string next_step;
};
std::vector<std::string> FormatEvolveProposeLines(const EvolveProposeModel& model,
                                                  const lubancode::cli::Theme& theme, int width);

// /evolve 域窄材料(HC-06 第三小批):观察账/演化目录的锚点与分层账。全
// 借用,绑定期一次配齐;字段与旧 SlashDispatchContext 同名同型。
struct EvolveCommandContext {
    const std::optional<std::string>* home_lubancode = nullptr;  // 演化目录锚点
    const std::filesystem::path* recordings_root = nullptr;      // 观察账的录制源
    lubancode::memory::ProjectMemory* project_memory = nullptr;  // 分层账;可空
    // TUI 排版批 5c:渲染段走 frame 三助手;空 = 没递(裸 CLI/测试),按终端
    // 能力探测起板(批 2 裁量 4,管道/重定向自然降 plain)。
    const lubancode::cli::Theme* theme = nullptr;
};

// /evolve 的分派位(命令注册表登册用)。
CommandFlow HandleSlashEvolve(const EvolveCommandContext& ctx,
                              const lubancode::cli::ParsedSlashCommand& parsed);

// CI 非交互入口:luban evolve test <candidate-dir> [--baseline <package-dir>]
// [--json]。人话或 JSON 打到 stdout,退出码按结果(全过 0/有 fail 1/夹具
// 缺失 2)。评测引擎与 /evolve test 同一枚 EvolutionCoordinator::TestDir。
int RunEvolveTestCommand(const EvolveTestArgs& args);

}  // namespace lubancode::app
