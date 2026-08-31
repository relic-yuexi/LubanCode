// ChannelIngressStore:入站耐久账(多渠道消息接入单阶段 2)。
//
// 唯一真源 docs/architecture/channels/message-contracts.md §3-4(去重键
// 三级、入站状态机、append-only journal)。落位 configuration.md §5:
//   <state_root>/<channel>/<account>/ingress/journal.jsonl      append-only
//   <state_root>/<channel>/<account>/ingress/dead-letter.jsonl  旁路账
//
// 记账规矩:
//   - evt 行:一枚入站事件的完整落盘。写成功即 durable(状态机的
//     received→durable 迁移被这一行吸收:落盘前不进任何队列,落盘失败
//     整个 Ingest 报错,调用方不得 ack sidecar)。
//   - tr 行:状态迁移(sid + to + reason + ts)。每次迁移只追加,快照
//     可由 journal 重建——"不拿一份可覆盖 JSON 当唯一真账"。
//   - replay:Open 时逐行重读,重建内存索引(去重键 → sid、delivery_id →
//     sid、状态)。半行(崩溃时写了一半)容错跳过并记 warning;中间坏行
//     同样跳过计数,不崩宿主——账还在,只是那一段查不到。
//
// 去重键三级(message-contracts.md §3,从上往下退):
//   1. provider_event_id 非空  -> p:<ch>:<acct>:<provider_event_id>(永久)
//   2. message_id 非空         -> m:<ch>:<acct>:<conv>:<message_id>(永久)
//   3. 指纹(前两级都空)      -> f:<sender>:<parts_sha256>:<time_bucket>
//      只作短窗去重(kFingerprintWindowMs 内查),不冒充永久 id。
//
// 依赖铁律沿 channel 库:标准库 + nlohmann::json + channel 内部件。
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "channel/types.hpp"

namespace lubancode::channel {

// ---------------------------------------------------------------------------
// 入站事件状态机(message-contracts.md §4)
// ---------------------------------------------------------------------------

enum class IngressEventState {
    Durable,              // evt 行落盘即 durable(received 瞬态被吸收)
    Authorized,
    Rejected,
    Routed,
    Queued,
    Running,
    Replied,
    CompletedWithoutReply,
    Delivered,
    DeliveryFailed,
    Archived,
    // 旁路终态。
    Duplicate,
    RateLimited,
    Unsupported,
    DeadLettered,
    Cancelled,
};

const char* IngressEventStateName(IngressEventState state);
std::optional<IngressEventState> IngressEventStateFromName(const std::string& name);
bool IsIngressTerminalState(IngressEventState state);
// 主线/旁路迁移合法性(状态机表驱动;终态无出边,Archived 是主线的坟)。
bool CanIngressTransition(IngressEventState from, IngressEventState to);

// ---------------------------------------------------------------------------
// 去重键
// ---------------------------------------------------------------------------

struct DedupeKey {
    int tier = 0;       // 1/2/3(message-contracts.md §3 的级数)
    std::string key;    // 稳定拼法(p:/m:/f: 前缀)
    std::int64_t window_until_ms = 0;  // tier 3 的短窗上界;tier 1/2 永久(0)
};

// 指纹短窗:10 分钟(message-contracts.md §3"短窗",首版钉死不进配置)。
inline constexpr std::int64_t kFingerprintWindowMs = 10 * 60 * 1000;
// 指纹时间桶粒度:10 秒(bucket = provider_at_ms / 10s)。
inline constexpr std::int64_t kFingerprintBucketMs = 10 * 1000;

// 算一枚事件的去重键。parts_sha256_out 可空;给时回填 parts 指纹(第三级
// 用的同一份 digest,诊断可复用)。
DedupeKey ComputeDedupeKey(const ChannelInboundEvent& event, std::string* parts_sha256_out);

// ---------------------------------------------------------------------------
// 账本
// ---------------------------------------------------------------------------

class ChannelIngressStore {
public:
    // 值语义被 mutex 禁:工厂返回 unique_ptr,装配方持指针。
    ChannelIngressStore() = default;
    ChannelIngressStore(const ChannelIngressStore&) = delete;
    ChannelIngressStore& operator=(const ChannelIngressStore&) = delete;

    struct Record {
        std::int64_t sid = 0;             // 账序号(journal 内单调,1 起)
        DedupeKey key;
        std::string parts_sha256;
        ChannelInboundEvent event;
        IngressEventState state = IngressEventState::Durable;
        std::string last_transition_reason;  // 最近一次迁移的稳定名(诊断/dead letter 用)
    };

    struct DeadLetterEntry {
        std::int64_t sid = 0;
        std::string channel_id;
        std::string account_id;
        std::string delivery_id;
        std::string reason;      // 稳定名
        std::int64_t at_ms = 0;
        ChannelInboundEvent event;  // 完整事件留档(dead letter 可 replay)
    };

    struct OpenResult {
        bool ok = false;
        std::string error;
        int skipped_lines = 0;  // replay 容错跳过的行数(半行/坏行)
    };

    // 打开(或新建)账号的 ingress 账。account_dir =
    // <state_root>/<channel>/<account>;journal 在其下 ingress/ 子目录。
    // 失败不落异常:错误经 result 带,返回的 store 处于"只读拒绝写"
    // (write_blocked)状态,调用方决定收还是弃。
    static std::unique_ptr<ChannelIngressStore> Open(const std::filesystem::path& account_dir,
                                                     std::string channel_id,
                                                     std::string account_id,
                                                     OpenResult* result = nullptr);

    bool write_blocked() const { return write_blocked_; }
    const std::string& last_error() const { return last_error_; }

    struct IngestOutcome {
        enum class Status { Accepted, Duplicate } status = Status::Accepted;
        std::int64_t sid = 0;             // Accepted:新账序号;Duplicate:原事件 sid
        DedupeKey key;
        bool ack = false;                 // 两种结局都该 ack sidecar(见下)
    };

    // 落一枚入站事件:durable 写盘 + 去重判定。重复事件不重新落 evt 行,
    // 只追加一行 duplicate 迁移到原事件账上。durable 写失败 → unexpected
    // (调用方不得 ack,sidecar 会按退避重发)。
    // Duplicate 也回 ack:事件已 durable 过,重送即重复,ack 让 sidecar
    // 清 spool(bridge-protocol.md §5 channel.inbound)。
    std::optional<IngestOutcome> Ingest(const ChannelInboundEvent& event);

    // 状态迁移(追加 tr 行)。非法迁移/未知 sid 报错。
    std::optional<std::string> Transition(std::int64_t sid, IngressEventState to,
                                          const std::string& reason);

    // 挪进 dead letter(状态旁路 + dead-letter.jsonl 留档)。
    std::optional<std::string> MoveToDeadLetter(std::int64_t sid, const std::string& reason,
                                                std::int64_t at_ms);

    // ---- 查询(快照,锁内拷贝) ----
    std::vector<Record> Records() const;
    std::optional<Record> FindBySid(std::int64_t sid) const;
    std::optional<std::int64_t> FindByDeliveryId(const std::string& delivery_id) const;
    // 状态计数(/channels、doctor 的水位投影)。
    std::map<std::string, std::size_t> StateCounts() const;
    std::size_t dead_letter_count() const;
    std::vector<DeadLetterEntry> DeadLetters() const;
    std::int64_t next_sid() const;

private:
    std::optional<std::string> AppendLine(const std::string& line);
    void ReplayLocked();

    std::filesystem::path journal_path_;
    std::filesystem::path dead_letter_path_;
    std::string channel_id_;
    std::string account_id_;
    bool write_blocked_ = false;
    std::string last_error_;
    int replayed_bad_lines_ = 0;  // replay 容错跳过的行数(OpenResult 带出)

    mutable std::mutex mutex_;
    std::int64_t next_sid_ = 1;
    std::deque<Record> records_;  // sid 升序
    // 去重索引:tier1/2 永久;tier3 由 ScanDuplicateLocked 现查(短窗)。
    std::unordered_map<std::string, std::int64_t> permanent_keys_;  // key -> sid
    std::unordered_map<std::string, std::int64_t> delivery_ids_;    // delivery_id -> sid
};

}  // namespace lubancode::channel
