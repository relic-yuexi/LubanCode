// Harness Exporter(One-shot 轨迹指定输出单,Harbor Harness 派生 JSONL):
// 把一场 session 的 canonical Journal 投影成外接评测 harness 用的便携
// JSONL——一行一枚 run,main 与 subagent/workflow 子流逐流各一行,靠
// parent_run_id 相连。
//
// 与 training-v1 的分工(单子 §二/§五):
//   - 这是 harness export,不是改写 canonical 落点:main.jsonl 等
//     canonical streams 照旧在 LubanCode workspace 下 append + flush,
//     恢复/验链/resume/usage/证据真值仍以它为准;
//   - 不复制 main.jsonl,也不把内部事件 schema、blob 相对路径与多 stream
//     布局泄漏成协议——目标文件是自洽的便携投影;
//   - training-v1 的四路筛选、训练授权与正文排除语义不硬套过来:失败/
//     partial 的 run 也如实产出,靠 outcome 分型区分;
//   - 引擎复用 projection 共享层(raw 扫/正文回读/隐私扫描)与
//     FoldStreamReplay(验链 + 折叠),与 training exporter 同一本底账。
//
// 三条铁律(继承 training exporter):
//   1. 纯读投影——不回写 Journal,不执行外部动作;
//   2. 只吃本地账,不联网(上传不属 exporter 职责);
//   3. 验链不过不装没事——stream 验链失败照样出一行(fail-closed 的
//      outcome=unknown 存根),绝不出 clean success。
//
// 字节形态:每行 CanonicalJsonDump。导出时刻(exported_at)是收据事实,
// 进 source(单子 §四点名要导出时间),故本导出不做 training-v1 那种
// 逐字节重放承诺——完整性由整文件 SHA-256 收据背书。
//
// 依赖铁律:trajectory 纯库,不 include app/cli/runtime。
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::trajectory {

// 行 schema 名与版本(单子 §四:首版暂定 lubancode.harness.trajectory)。
inline constexpr std::string_view kHarnessTrajectorySchema = "lubancode.harness.trajectory";
inline constexpr int kHarnessTrajectorySchemaVersion = 1;
inline constexpr std::string_view kHarnessExporterVersion = "harness-exporter-v1";
// trajectory export --format harness-v1 的格式名(cli_options 认的就是它)。
inline constexpr std::string_view kHarnessExportFormat = "harness-v1";

// ---------------------------------------------------------------------------
// 配置与指纹
// ---------------------------------------------------------------------------

struct HarnessExportOptions {
    // thinking 投影(单子 §四"按现有 trajectory 隐私策略"):默认不落正文,
    // 只留 thinking_ref + 省略缘由;显式开 true 才带正文(secret 照脱敏)。
    // 不得为外接 harness 临时绕开记录策略。
    bool include_thinking = false;
    // blob 正文回读上限:超限给 text_ref(sha256 + bytes + 缘由),不内联。
    std::uint64_t max_resolved_blob_bytes = 1024 * 1024;
    // 工具结果内联上限:超限给 head 摘要 + 引用(sha256/bytes),单文件
    // 不因一份巨型工具输出无上限膨胀(单子 §四)。
    std::uint64_t max_inline_tool_result_bytes = 64 * 1024;
    // 写盘前的磁盘余量门(§12.2 storage_exhausted 同款判据)。
    std::uint64_t min_free_bytes = 16 * 1024 * 1024;
};

// 配置指纹:canonical(选项投影)的 SHA-256,进每行的 source。进程退出码
// 是"这次导出的输入事实"不是配置,不掺进指纹(补导时退出码不可知,给
// null——诚实,不拿 0 冒充)。
std::string ComputeHarnessConfigHash(const HarnessExportOptions& options);

// ---------------------------------------------------------------------------
// outcome 分型(单子 §四:success/failure/partial/cancelled/budget_
// exhausted/unknown)。纯函数,单测直钉。
// ---------------------------------------------------------------------------

struct HarnessOutcomeInputs {
    bool fold_ok = false;             // stream 验链 + 折叠是否成功
    bool truncated_tail = false;      // 尾行截断(已验证前缀,§16.3)
    std::string run_terminal;         // "" | run.completed | run.failed | run.cancelled
    std::vector<std::string> turn_terminals;  // 逐 turn:turn.completed/failed/cancelled/""
    std::vector<std::string> failure_reasons;  // turn.failed/run.failed payload 的 reason
};

// 裁断次序:验链不过 -> unknown;截断/未收口 -> partial;失败(含预算)
// -> failure/budget_exhausted;取消 -> cancelled;其余全收口 -> success。
// 失败压过取消:先有真失败后有取消,以失败为准。
const char* ClassifyHarnessOutcome(const HarnessOutcomeInputs& inputs);

// ---------------------------------------------------------------------------
// 引擎(纯读;Build 不写盘,单测与预演用)
// ---------------------------------------------------------------------------

// 一行一枚 run:扫一间 session 目录(与 training exporter 同一份 stream
// 清单),逐流验链折叠、投影成一行 JSON。验链失败的流出一行
// outcome=unknown 的存根(不折正文),行数与 run 数对得上。
// process_exit_code:LubanCode 进程退出码(one-shot 路传 RunTurn 的
// status);补导路不可知,传 nullopt 落 null。
std::vector<nlohmann::json> BuildSessionHarnessRecords(
    const std::filesystem::path& session_dir, const HarnessExportOptions& options = {},
    std::optional<int> process_exit_code = std::nullopt);

struct HarnessExportReport {
    // 空 = 成功。稳定码:
    //   export.no_session_dir / export.no_streams
    //   export.storage_exhausted / export.write_failed
    //   export.internal_error
    std::string error_code;
    std::string message;
    std::filesystem::path target;  // 绝对落点(成功时)
    std::string session_id;        // session 目录名(收据用)
    std::string schema;            // kHarnessTrajectorySchema(收据用)
    int schema_version = 1;
    std::uint64_t records = 0;  // 行数 = run 数
    std::string sha256;         // 整文件
    bool ok() const { return error_code.empty(); }
};

// Build + 原子写:同目录唯一临时件 + flush 落盘 + 原子替换
// (ProcessCrashDurability)。用户显式点名目标,允许原子替换该一份文件;
// 失败不碰旧成品,不留半截 JSONL 冒充成品。
HarnessExportReport ExportSessionHarnessV1(const std::filesystem::path& session_dir,
                                            const std::filesystem::path& target_path,
                                            const HarnessExportOptions& options = {},
                                            std::optional<int> process_exit_code = std::nullopt);

}  // namespace lubancode::trajectory
