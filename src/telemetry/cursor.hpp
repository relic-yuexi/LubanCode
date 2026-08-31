// Telemetry 投影 cursor(端云协同可观测架构与 Telemetry 插件设计单
// §14.2,实施分期 T1"cursor 与 catch-up")。
//
// cursor 只回答"源事件投影到哪里"(§14.2):它不回答"云端收到了哪里"——
// 发送侧另有 ACK/tombstone 水位(T2 exporter 的账,本批只在 state.json
// 留 tombstone 簿)。规矩:
//   - cursor 原子替换(临时文件 + rename),永不全量覆写成半截;
//   - cursor 只在派生批次 durable 入 spool(seal 完)后推进;
//   - cursor 超前(Journal 里找不到 last_event_id)、hash 不合或 stream
//     换账,停止该 stream 并报错,不猜;
//   - projector 版本不兼容另开 generation(§27.2):generation 变了,旧
//     cursor 视为过期,从 Journal 头重投(新钥匙新 id,不与旧 spool 混账)。
//
// 文件布局:<telemetry-root>/cursors/<workspace_key>/<session_id>/<stream
// 路径把 / 换成 __>.json。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace lubancode::telemetry {

// 固定合同值(§14.2 cursor JSON 的 schema/version)。
inline constexpr std::string_view kCursorSchema = "lubancode.telemetry.cursor";
inline constexpr int kCursorVersion = 1;

struct StreamCursor {
    std::string workspace_key;
    std::string session_id;
    std::string stream;           // "main.jsonl" / "subagents/<run_id>.jsonl"
    std::string last_event_id;    // 空 = 尚未投影过(从 Journal 头开始)
    std::string last_event_hash;  // 该行的 event_hash(验账对不上即停)
    std::string projector_version;
    int projection_generation = 1;
    std::int64_t updated_at_ms = 0;
};

// cursor 文件名里的路径安全形:stream 里的 '/' 换 '__'。
std::string CursorFileStem(std::string_view stream);

// cursor 文件路径(cursors/<workspace>/<session>/<stem>.json)。目录不建,
// StoreCursor 自建。
std::filesystem::path CursorFilePath(const std::filesystem::path& cursors_root,
                                     std::string_view workspace_key, std::string_view session_id,
                                     std::string_view stream);

// 读 cursor:文件不存在回 nullopt(新 stream,从 Journal 头投);坏 JSON/
// 坏字段回错误码。error_code 空 = 正常。
std::optional<StreamCursor> LoadCursor(const std::filesystem::path& cursors_root,
                                       std::string_view workspace_key,
                                       std::string_view session_id, std::string_view stream,
                                       std::string* error_code);

// 原子写 cursor(建目录 + tmp + rename)。false = 写失败(调用方保持旧
// cursor 语义:下次补读同一窗口,靠 batch id 去重)。
bool StoreCursor(const std::filesystem::path& cursors_root, const StreamCursor& cursor);

}  // namespace lubancode::telemetry
