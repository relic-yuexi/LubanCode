// 新会话写 v3 的最小开关(P2 接线评估落地;单子 §1.5 大重构口径 +
// 分期 P2"读取侧按 v3 解析",P3 显示侧未齐,默认不切)。
//
// 开关语义:只管"新会话选哪种轨迹格式";老会话(已存在的 v2 目录)
// 永远照 v2 读写,与本开关无关。默认关 = 新会话仍写 v2。
//
// 接线点清单(实施留待 P3/后续棒,读取侧已就绪,P2 不强行接):
//
//   1. 开新会话(写侧入口)
//      src/runtime/trajectory_session.cpp 的延迟开卷装配(P0-C:正式
//      .jsonl 由首枚 run.started 提交事务独占建卷):开 = 在该处以
//      V3Writer::Start 建 sessions/<id>/<id>.jsonl,关 = 现行 v2
//      main.jsonl。这是唯一需要二选一的点。
//   2. 会话清单与目录发现(读回路;P3 已接)
//      src/trajectory/session_manager.cpp 与 session_index.cpp 并列识别
//      sessions/<id>/<id>.jsonl(首行 schemaVersion==3),识别助手即本件
//      的 FindV3SessionStream。两回路并存,按源目录格式分派。
//   3. resume 读回路(P3 已接)
//      v2 源照旧 FoldStreamReplay/effective conversation 投影;v3 源走
//      ReadV3Ledger + ProjectModelContext 链投影(session_manager.cpp
//      ResumeAsNew 内分派)。不迁移旧档(§1.5)。
//   4. subagent/工作流子账
//      src/runtime/trajectory_session.cpp 子账(subagents/<run>.jsonl)
//      同样受开关管辖:开 = 子账走 v3::SubagentSpawn 五步;工作流
//      workflow.jsonl 编排账不迁移(v3 只管 session 主账/子账)。
//   5. 显示层(纯 P3,已接终端)
//      终端 resume 重放吃 runtime 适配层的 RestoredHistoryView 投影
//      (src/runtime/trajectory_history_view.hpp,HistoryTimeline → 显示
//      DTO;cli 不直接碰 reader.hpp 的 C++ 结构);app-server/browser 的
//      旧史滚动、分页详情留后续棒。
//
// 判据已核已翻(2026-09-11):v3 Continue 全家福 + P2 读取侧矩阵全过;
// 显示侧(P3)吃上 HistoryTimeline;api/wire 四角色(P4)合同测试绿;
// 端到端验收矩阵 25 行 209 断言全绿,D1/D2/D3 三缺陷修复合入。
// 新会话默认写 v3;LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0 显式回 v2。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::trajectory::v3 {

// 新会话是否写 v3。环境变量 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=1 开;
// 其余任何值(含未设)关。每次调用现读,进程内不缓存——开关翻动只影响
// 之后开的新会话。
bool NewSessionV3WriteEnabled();

// ---------------------------------------------------------------------------
// 接线点 2 的目录发现助手(P3):v3 会话目录的并列识别。
//
// 认定规则:main.jsonl 在 = v2 布局(现行路一字不动);否则
// sessions/<id>/<id>.jsonl 存在且首行 schemaVersion==3 = v3 会话。
// 只读首行不整卷验链(验账归 ReadV3Ledger);识别不出给 nullopt,
// 调用方按 v2/损坏老路走,不在识别处猜。
// ---------------------------------------------------------------------------

// v3 主账流路径:<session_dir>/<目录名>.jsonl 且首行 schemaVersion==3。
std::optional<std::filesystem::path> FindV3SessionStream(const std::filesystem::path& session_dir);

// v3 主账首行(已解析 JSON;打不开/空文件/坏 JSON 给 nullopt)。
std::optional<nlohmann::json> ReadV3FirstLine(const std::filesystem::path& stream);

// v3 信封行 timestamp(ISO-8601 UTC,"2026-09-10T08:35:01.002Z")→ epoch
// 毫秒。认不动给 nullopt(排序回落最旧,不猜);只认 UTC 字面量,不做
// 时区换算——writer 落的就是 UTC。
std::optional<std::int64_t> ParseV3TimestampMs(const std::string& iso);

}  // namespace lubancode::trajectory::v3
