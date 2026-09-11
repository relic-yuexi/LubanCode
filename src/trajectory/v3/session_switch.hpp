// 新会话格式选择与目录发现。新会话默认写 v3；只有环境变量
// LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS 恰为 "0" 才回 v2。
// V3-LEGACY-01:旧写口与开关待退役；见 docs/development/v3-legacy-audit.md。
//
// 写侧由 TrajectorySessionLedger 选择 <id>.jsonl 或 main.jsonl。
// 读侧按源格式分派：v3 走 ReadV3Ledger/ProjectModelContext，v2 保留旧路。
// resume-as-new 不追加源文件；子代理随父场格式，Workflow 编排账另管。
// 终端历史投影和 AppServer 1.2 只读视图已接；完整 2.0 恢复协议另行推进。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::trajectory::v3 {

// 每次调用现读环境变量，不缓存。未设置或非 "0" 均开；只影响以后
// 新建会话的格式选择，不转换已有目录。
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
