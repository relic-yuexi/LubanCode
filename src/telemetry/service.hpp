// TelemetryService(端云协同可观测架构与 Telemetry 插件设计单 §26 生命
// 周期/§14 投影读路,实施分期 T1"TelemetryService 生命周期")。
//
// 本地常驻件:起停、committed wake、增量投影、有界队列、segmented spool、
// cursor 推进与 catch-up。T1 红线:不连真外部服务、不写真实密钥、T0 六件
// 合同不改(本件只是 T0 投影的消费者);T2 才有 exporter/consent/policy。
//
// 启动(§26.1,收窄到本地面):
//   ResolveTelemetryActivation(装配层已判 Active)
//   -> 打开/恢复 state.json(projection_key + generation 账)
//   -> open spool and recover(§18.5)
//   -> load cursors(懒:发现 stream 时读)
//   -> start projector worker(单线程;周期 tick 补丢掉的 wake,§14.1)
//   -> Ready
// 开启失败不抛、不拖死 Agent(§26.1 默认模式):服务进 degraded,状态面
// 报原因,Agent 照跑。
//
// projection_key 的归属(T0 identity.hpp 留给 T1 的口):由本件持有——
// 首次启动随机生成 64 字节,存 <root>/state.json,重启沿用(同一
// projector version 下 span id 稳定重建);projector version 不兼容时
// generation+1 并换新钥匙(§27.2 另开 generation,新 id 不与旧 spool 混
// 账)。钥匙不进日志、不进 spool、不进任何导出负载。
//
// 三层账(§14.2):projection cursor(本件 cursors/)、pending spool
//(spool/ 段)、export ACK 状态(state.json 的 tombstone 簿,T2 exporter
// 才真用;T1 只在对账时认它)。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "telemetry/activation.hpp"
#include "telemetry/batch_queue.hpp"
#include "telemetry/contract.hpp"
#include "telemetry/cursor.hpp"
#include "telemetry/exporter.hpp"
#include "telemetry/spool.hpp"
#include "telemetry/wake.hpp"

namespace lubancode::telemetry {

struct TelemetryServiceOptions {
    std::filesystem::path telemetry_root;  // <user-data>/telemetry(§18.1)
    ResourceInputs resource;               // 装配层采好(不含主机名/路径)
    DataClass data_class = DataClass::Metadata;
    BatchQueueOptions queue;
    SpoolOptions spool;
    // T2 出口(§19):endpoint 空 = 不出网(本地投影照跑,§7.1 收窄到
    // spool)。出口线程只在 configured 时起。
    OtlpExporterOptions exporter;
    bool spool_enabled = true;             // telemetry.spool.enabled:关 = 派生批即弃(不出网)
    std::int64_t tick_ms = 1000;         // 周期扫描(丢 wake 补账的兜底)
    std::int64_t flush_interval_ms = 500;  // spool 时间帽(§18.3 batch flush)
};

// 本地状态面(/telemetry 与 /doctor telemetry 的数据源,§24.3 收窄到
// T1 本地面:无 exporter/connector/policy 节)。
struct TelemetryServiceStatus {
    bool running = false;
    bool degraded = false;
    std::string degraded_reason;   // 稳定码(telemetry.*)
    int projection_generation = 1;
    std::string projector_version;
    std::int64_t started_at_ms = 0;
    std::uint64_t missed_wakes = 0;
    BatchQueueStats queue;
    SpoolStats spool;
    SpoolRecoveryReport recovery;
    // stream 账:每条被发现的 stream 的 cursor 位置/滞后/错误。
    struct StreamStatus {
        std::string workspace_key;
        std::string session_id;
        std::string stream_id;
        std::string last_event_id;
        std::uint64_t lag_events = 0;  // 已落账未投影的事件数(末趟口径)
        std::string error_code;        // 空 = 健康;非空 = 该 stream 已停
        bool final_flushed = false;
    };
    std::vector<StreamStatus> streams;
    // T2 出口面(§24.3:exporter endpoint(去 query/userinfo)、last export
    // success/error、出口计数)。endpoint 未配 = 全零 + configured=false。
    ExportStatusFace exporter;

    nlohmann::json ToJson() const;
};

// /doctor telemetry 的正文行(纯函数,单测钉格式;不联网)。
std::vector<std::string> FormatTelemetryDoctorLines(const TelemetryServiceStatus& status);
// /telemetry status 的正文行(同源,更简短)。
std::vector<std::string> FormatTelemetryStatusLines(const TelemetryServiceStatus& status);

class TelemetryService : public CommitObserver {
public:
    explicit TelemetryService(TelemetryServiceOptions options);
    ~TelemetryService() override;  // 未 Stop 则有界收场(§26.3)

    TelemetryService(const TelemetryService&) = delete;
    TelemetryService& operator=(const TelemetryService&) = delete;
    TelemetryService(TelemetryService&&) = delete;
    TelemetryService& operator=(TelemetryService&&) = delete;

    // §26.1 启动。false = 本地开不出(spool/目录/状态坏)——服务进
    // degraded,调用方照跑 Agent,状态面报原因。
    bool Start();

    // §26.3 关停:停收 wake -> 末趟 catch-up -> seal active -> 持久化
    // cursor/state -> 收线程。有界,不等任何网络(T1 本地无网络)。
    void Stop();

    // ---- CommitObserver(§25.3):非阻塞,只投 wake ----
    void Notify(const CommitWake& wake) noexcept override;

    // 注册一场 session 的账目位置(装配层在 ledger 开张后调;stream 发现
    // 由周期扫描在 session 目录内做——main.jsonl + subagents/*.jsonl,
    // 不扫全盘猜文件,§14.3)。
    void RegisterSession(const std::string& workspace_key, const std::string& session_id,
                         const std::filesystem::path& session_dir);

    // 状态面(本地读,无 IO 副作用)。
    TelemetryServiceStatus Status() const;

    // ---- T2 出口面(§24.2)----
    // pause/resume:停/复出口,本地投影与 spool 照常(§24.2 "pause 停出口,
    // 可继续按本地策略有限落 spool")。
    void SetExportPaused(bool paused);
    // flush:立即 seal active,推出口线程赶一趟,有界等(§24.2 "只尝试发送
    // sealed segment;不强制等公网无限久";§26.3 --telemetry-flush-timeout
    // 有硬上限)。回 true = 存量 sealed 批全出清(或出口本就没开)。
    bool Flush(std::int64_t bounded_ms);
    // 出口状态(轻量版,Status() 的 exporter 节同源)。
    ExportStatusFace ExportStatusFaceData() const;
    // §8.4 公网确认:授权当前 endpoint+数据档+脱敏版本(写 <root>/consent.json,
    // 并放行非回环出口);回 false = 落盘失败。回环 endpoint 不需要授权。
    bool GrantConsent();
    // spool clear(§24.2 删除动作):清空全部 sealed 段与 active.tmp。批账
    // 先落 retired watermark/tombstone(cursor 对账不报孤儿,§18.5),再删
    // 文件。回被清掉的 (段, 批) 数。
    std::pair<std::size_t, std::size_t> ClearSpool();
    // 撤回:删 consent 记录;非回环出口立即关门(telemetry.consent_required)。
    bool RevokeConsent();
    // consent 状态:granted | not_required(回环) | required。
    std::string ConsentState() const;
    // §24.2 "--probe 才对明配 endpoint 发无业务数据的探针"。出口没配回
    // nullopt。探针不碰 spool、不记账。const:/doctor 只读诊断面。
    std::optional<ExportAttempt> ProbeEndpoint() const;

    // T2 口(本批只做本地生命周期):ACK 批次 -> 删段;删失败的批记
    // tombstone 进 state.json(§18.2 防重复无限发)。
    void AckBatches(const std::vector<std::string>& batch_ids);

    const TelemetryServiceOptions& options() const { return options_; }

private:
    struct SessionEntry {
        std::string workspace_key;
        std::string session_id;
        std::filesystem::path session_dir;
    };
    struct StreamState {
        StreamCursor cursor;
        std::string error_code;  // 非空 = 该 stream 停投(§22.5/§14.2 不猜)
        std::uint64_t lag_events = 0;
        std::uintmax_t last_size = 0;  // 上趟文件大小(tick 兜底的省读判据)
        bool final_flushed = false;
    };
    struct PendingAdvance {
        StreamCursor cursor;
        std::uint64_t append_epoch = 0;  // 落 active 时的代;seal 过的代才许推
    };

    void WorkerLoop();
    void RunPass(bool final_flush);
    void ProjectStream(const SessionEntry& session, const std::string& stream_id,
                       bool final_flush);
    void DrainQueue();
    void AdvancePendingCursors();
    void DiscoverStreams(const SessionEntry& session);
    // ---- T2 出口线程(§19;业务线程不等网络,Notify 面零变化)----
    void ExportLoop();
    // 赶一趟出口:按段序逐批 POST;Accepted/Partial(有收)→ ACK 删段;
    // Retryable → 退避记账(§19.2 双限);Permanent → 关 endpoint 代。
    // 回 true = 本趟把现存 sealed 批全部出清(无剩、无在等退避)。
    bool RunExportPass();
    void EvaluateExportGateLocked();
    // state.json 的读写(§14.2 三层账的第三层:ACK/tombstone/清理水位)。
    bool LoadState();
    bool PersistState();
    std::string DeriveBatchId(const std::string& workspace_key, const std::string& session_id,
                              const std::string& stream_id, const std::string& last_event_id,
                              const char* signal, bool final_window) const;

    TelemetryServiceOptions options_;
    std::string projection_key_;
    std::string device_instance_id_;
    int projection_generation_ = 1;
    std::vector<std::pair<std::string, std::int64_t>> tombstones_;  // batch_id, acked_at
    // 已退场窗口的水位(ACK 删除或 TTL 清理覆盖到的 stream 端点,§18.5
    // "cursor 超前先查水位;有账 = 正常清理")。
    std::map<std::string, std::string> retired_watermarks_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> missed_wakes_{0};
    std::thread worker_;
    // wake 面:Notify 只碰这把锁(µs 级,不与投影竞争)。
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    std::set<std::string> pending_wakes_;  // 键 = ws|session|stream
    // 账面:sessions/streams/cursor(投影线程写,状态面读)。
    mutable std::mutex state_mutex_;
    std::map<std::string, SessionEntry> sessions_;  // 键 = ws|session
    std::map<std::string, StreamState> streams_;    // 键 = ws|session|stream
    // spool 面(单 writer:worker 线程;epoch 记"落 active 没 seal"的账)。
    std::unique_ptr<TelemetrySpool> spool_;
    BatchQueue queue_;
    std::map<std::string, PendingAdvance> pending_cursor_advances_;
    std::uint64_t append_epoch_ = 0;
    std::uint64_t durable_append_epoch_ = 0;
    std::uint64_t last_seen_seal_generation_ = 0;
    SpoolRecoveryReport recovery_;
    std::string degraded_reason_;
    std::int64_t started_at_ms_ = 0;
    // ---- T2 出口面 ----
    // spool 并发面:投影 worker(落盘/seal/cursor)与出口线程(读段/ACK
    // 删段)各持此锁过;锁序 spool -> state,export 永不嵌在 spool 里。
    mutable std::mutex spool_mutex_;
    mutable std::mutex export_mutex_;  // 出口账/退避表/门/pause
    std::condition_variable export_cv_;
    std::condition_variable flush_cv_;  // flush 等出清(有界)
    bool export_wake_ = false;          // export_mutex_
    bool export_paused_ = false;        // export_mutex_(§24.2 pause)
    std::string export_gate_reason_;    // export_mutex_(consent/https/永久错)
    struct BatchRetry {
        int attempts = 0;
        std::int64_t next_eligible_ms = 0;
    };
    std::map<std::string, BatchRetry> export_retry_;  // export_mutex_;键=batch_id
    ExportStatusFace export_stats_;                   // export_mutex_
    std::thread export_thread_;
    mutable std::atomic<bool> export_cancel_{false};  // 在传输请求的取消(§26.4;probe 只读面也要能清残留)
    std::unique_ptr<OtlpHttpExporter> exporter_;
    std::optional<ConsentStore> consent_store_;
};

// 装配工厂(§8.5 默认关闭硬验收的代码面):ResolveTelemetryActivation 判
// 非 Active 一律回 nullptr——不建目录、不起线程、零遥测副作用;note 回
// 人话原因(装配层可打可不打)。Active 才造服务并 Start。
struct TelemetryAssemblyInputs {
    bool config_telemetry = false;
    bool config_trajectory = false;
    TelemetryServiceOptions options;  // activation 过了才用得上
};
std::unique_ptr<TelemetryService> TryAssembleTelemetryService(
    const TelemetryAssemblyInputs& inputs, std::string* note);

}  // namespace lubancode::telemetry
