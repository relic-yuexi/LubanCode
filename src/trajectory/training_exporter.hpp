// Training Exporter(P0 新轨迹记录单 §十一,P0-5):把 Journal 投影成
// provider-neutral 训练样本。
//
// 四条铁律(§2.7 第 7 条/§十八):
//   1. 纯读投影——只吃 session 目录里的 JSONL 与 artifacts/ blob,不回写
//      Journal,不执行任何外部动作,不改 recorder 行为;
//   2. 只吃本地账,不联网(§12.3:上传、发布、同步不属 Exporter 职责);
//   3. 不造假——trajectory 关的会话没账可导,报空;无证据的成功进不了
//      success(§11.5 成功门逐条落);
//   4. 字节确定——同一 Journal、同一 exporter 配置,连导两次输出逐字节
//      一致(§16.5);episode 行与 manifest 全走 CanonicalJsonDump,任何
//      墙钟、I/O 序、目录迭代序都不进产物(manifest 的"生成时间"锚在
//      源账末事件的 wall_time_ms 上,是 Journal 事实,不是导出时刻)。
//
// episode 单位(§11.1):默认一枚外层 turn 一条 episode;父文件里的
// subagent.result.accepted / workflow.node.completed 一类边界只进 step
// metadata,子账正文绝不内联——同一段子轨迹只在它自己的文件里导一次。
// 现运行时未写 workflow/goal/loop 编排流,其 run 级单位随那些事件词汇
// 落账后另补(P0-6+),本版不造空壳。
//
// 训练导出授权(§12.3):auto_export_training 默认 false;本件只由显式
// CLI 命令触发,manifest 记授权来源与 exporter config hash。
//
// 依赖铁律:trajectory 纯库,不 include app/cli/runtime。secret 扫描复用
// insights 层冻结的扫描器(Token 账本单 A0),不在 trajectory 重造第二份
// 模式表(telemetry redactor 同款先例)。
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

// episode schema 名与版本(§11.1)。exporter schema 单独版本化(§8.4)。
inline constexpr std::string_view kTrainingEpisodeSchema = "lubancode.training.episode";
inline constexpr int kTrainingEpisodeSchemaVersion = 1;
// 数据集 manifest 的 schema 名(§3.1 exports/training-v1/manifest.json)。
inline constexpr std::string_view kTrainingDatasetSchema = "lubancode.training.dataset";
inline constexpr int kTrainingDatasetSchemaVersion = 1;
inline constexpr std::string_view kTrainingExporterVersion = "trajectory-exporter-v1";
// 唯一实现的目标格式(§十四 `--format training-v1`)。
inline constexpr std::string_view kTrainingExportFormat = "training-v1";

// 四路(§11.3):由独立质量轴裁断,不是一只 clean 布尔。
enum class EpisodeRoute { Success, Failure, Partial, Excluded };
const char* EpisodeRouteName(EpisodeRoute route);
std::optional<EpisodeRoute> EpisodeRouteFromName(std::string_view name);
// 四路文件名:success.jsonl / failure.jsonl / partial.jsonl / excluded.jsonl。
const char* EpisodeRouteFileName(EpisodeRoute route);

// ---------------------------------------------------------------------------
// 配置与指纹(§11.7)
// ---------------------------------------------------------------------------

struct TrainingExportOptions {
    // §11.6:provider 明确返回的 thinking/reasoning 默认剔除;开 true 才进
    // messages,manifest 同步写策略。
    bool include_reasoning = false;
    // blob 正文回读上限:超限的 text_ref 不内联(episode 判 excluded,
    // structure=blob_oversized),导出件不因一份巨型工具输出撑爆。
    std::uint64_t max_resolved_blob_bytes = 1024 * 1024;
    // 写盘前的磁盘余量门(§12.2 storage_exhausted 同款判据;16 MiB 是
    // §13 journal_emergency_reserve_bytes 的首版起始值)。
    std::uint64_t min_free_bytes = 16 * 1024 * 1024;
};

// 配置指纹:canonical(选项投影)的 SHA-256。进 episode fingerprint 与
// manifest,同账不同配置必出不同指纹(§11.7)。
std::string ComputeExporterConfigHash(const TrainingExportOptions& options);

// episode_fingerprint = SHA256(run_id + turn_key + journal_last_hash +
// exporter_config_hash)(§11.7 原式)。turn_key 是 turn id;数据集只防
// 同一 run 重复导出,不同 run 正文相同也不自动合并。
std::string ComputeEpisodeFingerprint(std::string_view run_id, std::string_view turn_key,
                                      std::string_view journal_last_hash,
                                      std::string_view exporter_config_hash);

// ---------------------------------------------------------------------------
// 质量轴与四路裁断(§11.4/§11.3)
// ---------------------------------------------------------------------------

// 裁断输入(轴值全为稳定码,单测直钉)。
struct EpisodeRouting {
    // valid | truncated_tail | verify_failed | unsupported | blob_missing |
    // blob_oversized
    std::string structure = "valid";
    // passed | excluded(命中隐私扫描)
    std::string privacy = "passed";
    // exact_offline | source_exact | input_only | blocked | unknown
    // (§9.2 四档投影:exact→exact_offline,source_exact_environment_partial
    // →source_exact;P0-5 训练门只放行前两档)
    std::string replayability = "unknown";
    // complete | incomplete(turn 与 run 都正常收口才算 complete)
    std::string completeness = "incomplete";
    // verified | unverified | stale | insufficient
    std::string verification = "unverified";
    // "" | succeeded | failed | cancelled(outcome.assessed 的裁决;空 =
    // 本 turn 没有 assessed 事件,turn.completed 自称的不算)
    std::string assessed_outcome;
    // "" | turn.completed | turn.failed | turn.cancelled
    std::string turn_terminal;
    // "" | run.completed | run.failed | run.cancelled
    std::string run_terminal;
    bool unknown_side_effect = false;
};

// 纯函数:独立质量轴 -> 四路 + 缘由(稳定码,进 quality.reasons 与 manifest
// 的 exclusion_reasons 账)。裁断次序照 §11.5 成功门:结构、隐私、replay
// 档、unknown side effect 先挡;complete 且 verified 且 succeeded 才进
// success;已知失败/取消且收口进 failure;未收口(预算耗尽、人工打断、
// 运行未收口)进 partial;任务成了却没证据只进 excluded/unverified。
EpisodeRoute DecideEpisodeRoute(const EpisodeRouting& routing, std::vector<std::string>* reasons);

// ---------------------------------------------------------------------------
// 产物
// ---------------------------------------------------------------------------

// 一枚裁断完的 episode。episode 即 §11.1 全形 JSON(canonical dump 后就是
// 写盘行);privacy 命中的 episode 正文整包扣下(messages 置空),只带
// privacy_findings 的稳定码与 source event id——excluded.jsonl 不许成为
// 泄密出口(§十二"命中后进 excluded,报告 source event id")。
struct ExportedEpisode {
    nlohmann::json episode;
    EpisodeRoute route = EpisodeRoute::Excluded;
    std::vector<std::string> reasons;  // 裁断缘由(稳定码)
    std::string fingerprint;
    std::string run_id;
    std::string turn_key;  // turn id(episode_id 的后半)
};

struct TrainingExportReport {
    // 空 = 成功。稳定码:
    //   export.no_session_dir / export.no_streams
    //   export.storage_exhausted / export.write_failed
    //   export.internal_error
    std::string error_code;
    std::string message;
    std::filesystem::path export_dir;      // <session>/exports/training-v1
    std::map<std::string, std::uint64_t> counts;  // 路由名 -> episode 数
    // §13.1"export episode counts and exclusion reasons":稳定码 -> 计数。
    std::map<std::string, std::uint64_t> exclusion_reasons;
    std::uint64_t streams = 0;         // 参与投影的 JSONL 份数
    std::uint64_t episodes = 0;        // 四路合计
    bool ok() const { return error_code.empty(); }
};

// ---------------------------------------------------------------------------
// 引擎(纯读;Build 不写盘,单测与预演用)
// ---------------------------------------------------------------------------

// 扫一间 session 目录(main + subagents + workflow/node + goals/loops,
// VerifySessionDir 同一套清单),逐 stream 验链折叠、逐 turn 编 episode、
// 逐枚裁断四路。不写任何文件。链断/尾行截断的 stream 出一枚 run 级
// excluded 存根(structure.verify_failed / structure.truncated_tail),不
// 折叠其正文。
std::vector<ExportedEpisode> BuildSessionTrainingEpisodes(
    const std::filesystem::path& session_dir, const TrainingExportOptions& options = {});

// Build + 写盘:<session>/exports/training-v1/{success,failure,partial,
// excluded}.jsonl + manifest.json。每份文件临时件 + 原子 rename;写前过
// 磁盘余量门(§12.2),不足即 export.storage_exhausted,一个字节不写。
TrainingExportReport ExportSessionTrainingV1(const std::filesystem::path& session_dir,
                                             const TrainingExportOptions& options = {});

// export-workspace(§十四):逐 session 各导进各自 exports/training-v1/
// (§3.1 只定义 session 级 exports;跨 session 合并数据集不属本版),报告
// 汇总。sessions/ 不存在或一场可导的都没有时报错不造假。
TrainingExportReport ExportWorkspaceTrainingV1(const std::filesystem::path& workspace_dir,
                                               const TrainingExportOptions& options = {});

}  // namespace lubancode::trajectory
