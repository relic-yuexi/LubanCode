// TelemetrySpool(端云协同可观测架构与 Telemetry 插件设计单 §18,
// 实施分期 T1"segmented spool、crash recovery、容量/TTL")。
//
// 目录(§18.1,本件只管后两格;consent/policy/cursors/state 归 Service):
//   <telemetry-root>/spool/
//     active.tmp                  活动段(只 spool writer 可写)
//     seg-<8位十进制>.otlpjson   封口段:每行一只 durable batch(NDJSON)
//     seg-<同号>.meta.json        段账:batch 范围/哈希/版本/数据档
//   <telemetry-root>/quarantine/  半批与坏段(不无限增长,首版只计账)
//
// 段的生命周期(§18.2):
//   append 批 → active.tmp 追加一行 + fflush(ProcessCrash 档耐久);
//   达字节帽/条目帽/时间帽 → seal:fsync → rename 成 seg-NNNNNNNN.otlpjson
//   → meta(tmp+rename)→ 回段号;
//   cursor 只在 seal 完成后由 Service 推进(active.tmp 里的窗口不算 durable)。
//
// 崩溃恢复(§18.5):active.tmp 启动时逐行验边界;完整批次照 seal,半批/
// 坏行移 quarantine。段 payload 在而 meta 丢的,从 payload 行重造 meta
//(行里带着全部对账字段,不丢数据)。spool 比 cursor 多:允许重发,靠
// batch id 去重——Coverage() 把每段每批的窗口端点报给 Service 对账。
//
// 交付语义(§18.6):at-least-once;ACK/删除失败留 tombstone 是 T2 exporter
// 的账,本件只提供 AckBatches(删段 + 报失败)的本地原语。
//
// 满盘(§18.4):按账龄清(过 TTL 的 → 最老的),仍超帽 → spool_degraded:
// 停收新批、计数、/doctor telemetry 报红——绝不为保遥测挤爆用户磁盘。
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "telemetry/batch_queue.hpp"
#include "telemetry/contract.hpp"

namespace lubancode::telemetry {

// 段账固定合同值。
inline constexpr std::string_view kSegmentMetaSchema = "lubancode.telemetry.segment";
inline constexpr int kSegmentMetaVersion = 1;

struct SpoolOptions {
    std::uint64_t segment_bytes_cap = 4 * 1024 * 1024;   // §18.3 起点(1-8 MiB)
    std::size_t segment_items_cap = 256;
    std::uint64_t total_bytes_cap = 256 * 1024 * 1024;   // §18.3 起点(128-512 MiB)
    std::int64_t max_age_ms = 48 * 3600 * 1000;          // §18.3 起点(24-72 h)
};

// 段内一行:一只 durable batch(编码好的 OTLP 请求体随行落盘)。行自述
// 版本与数据档——meta 丢了也能从行重造(§18.5 崩溃恢复不丢账)。
struct SpoolBatchRecord {
    std::string batch_id;
    std::string signal;  // "traces" | "metrics"
    Priority priority = Priority::P2;
    std::string workspace_key;
    std::string session_id;
    std::string stream_id;
    std::string first_event_id;
    std::string last_event_id;
    std::string last_event_hash;
    bool final_window = false;
    DataClass data_class = DataClass::Metadata;
    std::string projector_version;
    std::string redaction_version;
    int telemetry_schema_version = kTelemetrySchemaVersion;
    int projection_generation = 1;
    nlohmann::json payload;  // OTLP 请求 JSON(已脱敏已编码)

    nlohmann::json ToJson() const;
    static std::optional<SpoolBatchRecord> FromJson(const nlohmann::json& json);
};

// 段账里的批索引(payload 之外的对账面;orphan 段从 payload 行重造)。
struct SegmentBatchIndex {
    std::string batch_id;
    std::string signal;
    std::string sha256;  // 该行(payload 线文本)的 SHA-256
    std::string workspace_key;
    std::string session_id;
    std::string stream_id;
    std::string first_event_id;
    std::string last_event_id;
    std::string last_event_hash;
    Priority priority = Priority::P2;
    bool final_window = false;
    int projection_generation = 1;  // 归属投影代(§27.2:换代不混账)
};

// 段账(§18.2 meta):batch 范围、哈希、数据档、各版本。
struct SealedSegment {
    std::uint64_t segment_id = 0;
    std::int64_t created_at_ms = 0;
    std::uint64_t bytes = 0;
    DataClass data_class = DataClass::Metadata;
    std::string projector_version;
    std::string redaction_version;
    int telemetry_schema_version = kTelemetrySchemaVersion;
    int projection_generation = 1;
    std::vector<SegmentBatchIndex> batches;

    nlohmann::json MetaToJson() const;
    static std::optional<SealedSegment> MetaFromJson(const nlohmann::json& json,
                                                     std::uint64_t segment_id);
};

// 一个 stream 的已 durable 覆盖端点(对账用;带投影代——换代后旧覆盖
// 不许把新投影的窗口跳过去,§27.2)。
struct StreamCoverage {
    std::string last_event_id;
    std::string last_event_hash;
    bool final_window = false;
    int projection_generation = 1;
};

struct SpoolStats {
    std::uint64_t bytes = 0;
    std::size_t segments = 0;
    std::int64_t oldest_age_ms = -1;   // -1 = 无段
    std::uint64_t active_batches = 0;  // active.tmp 里未 seal 的批
    std::uint64_t quarantined_total = 0;
    std::uint64_t cleaned_segments_total = 0;
    std::uint64_t cleaned_bytes_total = 0;
    bool degraded = false;             // §18.4 spool_degraded
    std::string last_error_code;

    nlohmann::json ToJson() const;
};

struct SpoolRecoveryReport {
    std::size_t sealed_from_active = 0;   // active.tmp 完整批次补 seal 的段数
    std::size_t quarantined_batches = 0;  // 半批/坏行移 quarantine 的行数
    std::size_t orphan_segments = 0;      // meta 丢、从 payload 重造的段数
    std::string error_code;
};

class TelemetrySpool {
public:
    TelemetrySpool(std::filesystem::path spool_dir, std::filesystem::path quarantine_dir,
                   SpoolOptions options);
    ~TelemetrySpool();  // 关 active 句柄(未 seal 的半批留在原地,等恢复)

    TelemetrySpool(const TelemetrySpool&) = delete;
    TelemetrySpool& operator=(const TelemetrySpool&) = delete;

    // 打开 + 恢复(§18.5):建目录、扫段、验 active.tmp。失败不抛——回报告,
    // degraded 状态照给,Agent 照跑。
    SpoolRecoveryReport OpenAndRecover(std::int64_t now_ms);

    // 追加一只批进 active.tmp;达字节/条目帽自动 seal(时间帽由 Service 的
    // 周期 SealIfDue 触发)。false = IO 失败或 degraded(批被拒,Service 记账)。
    bool AppendBatch(const SpoolBatchRecord& record, std::int64_t now_ms);

    // 时间帽封口(§18.2)。回是否封了。
    bool SealIfDue(std::int64_t now_ms, std::int64_t flush_interval_ms);

    // 立即 seal(关停路 §26.3"seal active spool")。
    bool SealNow(std::int64_t now_ms);

    // ACK 原语(T2 exporter 调;T1 供生命周期测试):删只含已 ACK 批的段,
    // 段里还有未 ACK 批则不动。回被删的段号;删失败进 last_error_code
    //(调用方记 tombstone,防重复无限发——§18.2)。
    std::vector<std::uint64_t> AckBatches(const std::vector<std::string>& batch_ids);

    // 容量/TTL 清理(§18.4)。开张与每次 seal 后调。
    void Cleanup(std::int64_t now_ms);

    // stream -> durable 覆盖端点(cursor 对账与 batch id 去重用)。
    std::map<std::string, StreamCoverage> Coverage() const;  // 键 = ws|session|stream

    // 被容量/TTL 清理删掉的段曾覆盖到的 stream 端点(§18.5 "cursor 超前
    // 先查水位;有账 = 正常清理"的清理半账;ACK 半账归 Service 的
    // tombstone 簿)。只记本进程内清掉的——跨进程持久化由 Service 落
    // state.json。
    const std::map<std::string, StreamCoverage>& CleanedCoverage() const {
        return cleaned_coverage_;
    }

    // batch id 是否已在 durable 段里(去重)。
    bool HasBatch(const std::string& batch_id) const;

    const std::vector<SealedSegment>& sealed_segments() const { return sealed_; }
    SpoolStats Stats(std::int64_t now_ms) const;
    const SpoolOptions& options() const { return options_; }
    const SpoolRecoveryReport& recovery() const { return recovery_; }
    // seal 代数:每成功封一段 +1。Service 拿它对表"落 active 的批是否已
    // durable"——cursor 只许推过已 seal 的窗口(§14.2)。
    std::uint64_t seal_generation() const { return seal_generation_; }

private:
    bool OpenActive();
    bool SealLocked(std::int64_t now_ms);
    std::uint64_t NextSegmentId() const;
    void RebuildCoverage();
    void RecomputeBytes();
    // active.tmp 的恢复:验行边界、补 seal、半批移 quarantine。
    void RecoverActive(std::int64_t now_ms, SpoolRecoveryReport* report);
    // payload 在 meta 丢的段:从 payload 行重造 meta。
    bool RebuildSegmentMeta(const std::filesystem::path& payload_path, std::uint64_t segment_id);

    std::filesystem::path spool_dir_;
    std::filesystem::path quarantine_dir_;
    SpoolOptions options_;
    std::FILE* active_ = nullptr;      // active.tmp 句柄
    std::uint64_t active_bytes_ = 0;
    std::size_t active_items_ = 0;
    std::int64_t active_opened_ms_ = 0;
    std::vector<SealedSegment> sealed_;
    std::map<std::string, StreamCoverage> coverage_;
    std::map<std::string, StreamCoverage> cleaned_coverage_;
    std::uint64_t seal_generation_ = 0;
    std::uint64_t total_bytes_ = 0;
    SpoolStats stats_;
    SpoolRecoveryReport recovery_;
};

}  // namespace lubancode::telemetry
